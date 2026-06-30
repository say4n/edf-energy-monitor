#include <esp_sntp.h>

void syncTime() {
  LOGI("syncTime start");
  
  // reset sync status to ensure we wait for a fresh sync if needed
  sntp_stop();
  
  setenv("TZ", TZ_LONDON, 1);
  tzset();
  
  configTzTime(TZ_LONDON, "pool.ntp.org", "time.google.com");
  
  unsigned long start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - start < 15000) {
    delay(250);
  }
  
  LOGI("syncTime complete now=%ld valid=%d synced=%d", static_cast<long>(nowUtc()), nowUtc() >= 1700000000, sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
}

void handleRoot() {
  LOGI("HTTP / requested configured=%d has_data=%d", config.configured, rtcHasData);
  String body;
  body += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  body += F("<title>EDF Paper Color</title><style>body{font-family:system-ui;margin:24px;max-width:720px}label{display:block;margin-top:12px}input{width:100%;padding:10px;font-size:16px}button{margin-top:16px;padding:10px 14px;font-size:16px}code{background:#eee;padding:2px 4px}</style></head><body>");
  body += F("<h1>EDF Paper Color</h1>");
  body += F("<p>Status: ");
  body += config.configured ? F("configured") : F("setup required");
  body += F("</p>");
  if (rtcLastError[0] != '\0') {
    body += F("<p><strong>Error:</strong> ");
    body += htmlEscape(rtcLastError);
    body += F("</p>");
  }
  if (config.configured) {
    body += F("<p>Account: <code>");
    body += htmlEscape(config.accountNumber);
    body += F("</code></p>");
    body += F("<p>Electricity: ");
    body += config.electricity.present ? htmlEscape(config.electricity.pointId + " / " + config.electricity.serial) : F("not discovered");
    body += F("</p><p>Gas: ");
    body += config.gas.present ? htmlEscape(config.gas.pointId + " / " + config.gas.serial) : F("not discovered");
    body += F("</p>");
    body += F("<form method='post' action='/refresh'><button>Refresh now</button></form>");
    body += F("<form method='post' action='/reset'><button>Reset setup</button></form>");
  }
  body += F("<h2>Setup</h2><form method='post' action='/setup'>");
  body += F("<label>EDF email<input name='email' type='email' required value='");
  body += htmlEscape(String(app_config::edfEmail));
  body += F("'></label>");
  body += F("<label>EDF password<input name='password' type='password' required></label>");
  body += F("<label>EDF account number<input name='account' placeholder='A-AAAA1111' required value='");
  body += htmlEscape(String(app_config::edfAccount));
  body += F("'></label>");
  body += F("<button>Save EDF account</button></form></body></html>");
  server.send(200, "text/html", body);
}

bool provisionEdfAccount(const String& email, const String& password, const String& account, bool drawProgress) {
  LOGI("provisionEdfAccount start source=%s email_present=%d password_present=%d account=%s",
       drawProgress ? "web" : "setup-header", email.length() > 0, password.length() > 0, account.c_str());

  String refreshToken;
  if (!obtainRefreshToken(email, password, &refreshToken)) {
    LOGW("setup failed during token acquisition account=%s", account.c_str());
    if (drawProgress) drawStatusScreen("Setup failed", "Open the web page and try again.");
    return false;
  }

  config.refreshToken = refreshToken;
  tokenState = TokenState {};
  tokenState.refreshToken = refreshToken;

  if (!discoverAccount(account)) {
    LOGW("setup failed during account discovery account=%s", account.c_str());
    if (drawProgress) drawStatusScreen("Setup failed", "Account discovery failed.");
    return false;
  }

  setupMode = false;
  bool refreshed = refreshData();
  if (rtcHasData) {
    drawDashboard();
  } else if (drawProgress) {
    drawStatusScreen("Setup saved", refreshed ? "No energy data returned." : "Refresh failed. G1 retries.");
  }
  LOGI("provisionEdfAccount complete account=%s refreshed=%d", account.c_str(), refreshed);
  return refreshed;
}

void handleSetup() {
  LOGI("HTTP /setup requested");
  String email = server.arg("email");
  String password = server.arg("password");
  String account = server.arg("account");
  email.trim();
  account.trim();

  if (email.length() == 0 || password.length() == 0 || account.length() == 0) {
    LOGW("setup rejected due to missing fields email=%d password=%d account=%d",
         email.length() > 0, password.length() > 0, account.length() > 0);
    server.send(400, "text/plain", "Missing setup fields");
    return;
  }

  bool refreshed = provisionEdfAccount(email, password, account, true);
  if (!config.configured) {
    server.send(500, "text/plain", String("Setup failed: ") + rtcLastError);
    return;
  }
  LOGI("setup complete account=%s refreshed=%d", account.c_str(), refreshed);
  server.send(200, "text/plain", refreshed ? "Setup complete. Device refreshed." : String("Setup saved, refresh failed: ") + rtcLastError);
}

void handleRefresh() {
  LOGI("HTTP /refresh requested configured=%d", config.configured);
  if (!config.configured) {
    server.send(409, "text/plain", "Setup required");
    return;
  }
  bool ok = refreshData();
  if (rtcHasData) {
    drawDashboard();
  } else {
    drawStatusScreen("Refresh failed", "No energy data available.");
  }
  LOGI("HTTP /refresh complete ok=%d", ok);
  server.send(ok ? 200 : 500, "text/plain", ok ? "Refreshed" : String("Refresh failed: ") + rtcLastError);
}

void handleReset() {
  LOGW("HTTP /reset requested");
  clearConfig();
  tokenState = TokenState {};
  setupMode = true;
  server.send(200, "text/plain", "Setup cleared");
  drawStatusScreen("Setup required", "Open the web page to configure EDF.");
}

void handleDebugApi() {
  if (!config.configured) {
    server.send(409, "text/plain", "Setup required");
    return;
  }
  time_t to = nowUtc();
  time_t from = to - 7 * 86400; // last 7 days instead of 24h, to ensure we hit delayed data
  String url = String(EDF_BASE_URL) + "/v1/electricity-meter-points/" + config.electricity.pointId +
               "/meters/" + config.electricity.serial + "/consumption/?period_from=" + isoUtc(from) +
               "&period_to=" + isoUtc(to) + "&page_size=10";
  
  refreshAccessToken();
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("User-Agent", EDF_USER_AGENT);
  http.addHeader("Authorization", authHeader());
  
  int code = http.GET();
  String payload = http.getString();
  http.end();

  String debugResponse = "{\n";
  debugResponse += "\"esp32_time\": \"" + isoUtc(to) + "\",\n";
  debugResponse += "\"query_url\": \"" + url + "\",\n";
  debugResponse += "\"http_code\": " + String(code) + ",\n";
  debugResponse += "\"raw_response\": " + payload + "\n";
  debugResponse += "}";

  server.send(200, "application/json", debugResponse);
}

void startWebServer() {
  if (webStarted) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/setup", HTTP_POST, handleSetup);
  server.on("/refresh", HTTP_POST, handleRefresh);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/debug-api", HTTP_GET, handleDebugApi);
  server.begin();
  webStarted = true;
  LOGI("web server started ip=%s", WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "offline");
}
