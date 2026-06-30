bool postJson(const String& url, const String& payload, JsonDocument& response, const String& auth = "", const String& context = "papercolor") {
  LOGI("HTTP POST start context=%s url=%s payload_bytes=%u auth_present=%d",
       context.c_str(), url.c_str(), payload.length(), auth.length() > 0);
  if (!ensureWifiConnected(context.c_str())) {
    ledPulse(255, 0, 0, 2);
    setLastError("Wi-Fi unavailable for HTTP POST");
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(3);
  if (!http.begin(client, url)) {
    ledPulse(255, 0, 0, 2);
    setLastError("HTTP begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", EDF_USER_AGENT);
  http.addHeader(EDF_CONTEXT_HEADER, context);
  if (auth.length() > 0) http.addHeader("Authorization", auth);

  int code = http.POST(payload);
  LOGI("HTTP POST complete context=%s status=%d", context.c_str(), code);
  if (code < 200 || code >= 300) {
    ledPulse(255, 0, 0, 2);
    setLastError("HTTP POST failed: " + String(code));
    http.end();
    return false;
  }

  DeserializationError err = deserializeJson(response, http.getStream());
  http.end();
  if (err) {
    ledPulse(255, 0, 0, 2);
    setLastError("JSON parse failed: " + String(err.c_str()));
    return false;
  }
  LOGI("HTTP POST parsed context=%s", context.c_str());
  return true;
}

bool getJson(const String& url, JsonDocument& response, const String& auth = "", const String& context = "papercolor", JsonDocument* filter = nullptr) {
  LOGI("HTTP GET start context=%s url=%s auth_present=%d", context.c_str(), url.c_str(), auth.length() > 0);
  if (!ensureWifiConnected(context.c_str())) {
    ledPulse(255, 0, 0, 2);
    setLastError("Wi-Fi unavailable for HTTP GET");
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(3);
  if (!http.begin(client, url)) {
    ledPulse(255, 0, 0, 2);
    setLastError("HTTP begin failed");
    return false;
  }
  http.addHeader("User-Agent", EDF_USER_AGENT);
  http.addHeader(EDF_CONTEXT_HEADER, context);
  if (auth.length() > 0) http.addHeader("Authorization", auth);

  int code = http.GET();
  int payloadSize = http.getSize();
  LOGI("HTTP GET complete context=%s status=%d content_length=%d filtered=%d free_heap=%u",
       context.c_str(), code, payloadSize, filter != nullptr, ESP.getFreeHeap());
  if (code < 200 || code >= 300) {
    ledPulse(255, 0, 0, 2);
    setLastError("HTTP GET failed: " + String(code));
    http.end();
    return false;
  }

  DeserializationError err = filter == nullptr
    ? deserializeJson(response, http.getStream())
    : deserializeJson(response, http.getStream(), DeserializationOption::Filter(*filter));
  http.end();
  if (err) {
    ledPulse(255, 0, 0, 2);
    setLastError("JSON parse failed: " + String(err.c_str()));
    return false;
  }
  LOGI("HTTP GET parsed context=%s free_heap=%u", context.c_str(), ESP.getFreeHeap());
  return true;
}
