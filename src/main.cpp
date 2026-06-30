#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>
#include <M5PM1.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <qrcode.h>
#include <time.h>

#if __has_include("setup.h")
#include "setup.h"
#define HAS_LOCAL_WIFI_SECRETS 1
#else
#warning "include/setup.h not found; using placeholder Wi-Fi credentials"
#define WIFI_SSID "replace-me"
#define WIFI_PASSWORD "replace-me"
#define HAS_LOCAL_WIFI_SECRETS 0
#endif

#ifndef WIFI_HIDDEN
#define WIFI_HIDDEN 0
#endif

#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL 0
#endif

#ifndef WIFI_BSSID
#define WIFI_BSSID ""
#endif

#ifndef EDF_EMAIL
#define EDF_EMAIL ""
#endif

#ifndef EDF_PASSWORD
#define EDF_PASSWORD ""
#endif

#ifndef EDF_ACCOUNT
#define EDF_ACCOUNT ""
#endif

namespace {

#define LOGI(fmt, ...) logLine("I", __func__, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) logLine("W", __func__, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) logLine("E", __func__, fmt, ##__VA_ARGS__)

constexpr const char* EDF_BASE_URL = "https://api.edfgb-kraken.energy";
constexpr const char* GRAPHQL_URL = "https://api.edfgb-kraken.energy/v1/graphql/";
constexpr const char* TZ_LONDON = "GMT0BST,M3.5.0/1,M10.5.0/2";
constexpr const char* EDF_USER_AGENT = "stevekirtley-ha-edf-energy/embedded-papercolor";
constexpr const char* EDF_CONTEXT_HEADER = "Ha-Integration-Context";

constexpr gpio_num_t BTN_UP_PIN = GPIO_NUM_10;      // G10
constexpr gpio_num_t BTN_DOWN_PIN = GPIO_NUM_9;     // G9
constexpr gpio_num_t BTN_REFRESH_PIN = GPIO_NUM_1;  // G1
constexpr uint64_t ONE_HOUR_US = 60ULL * 60ULL * 1000000ULL;

constexpr int MAX_MONTH_DAYS = 31;
constexpr int YEAR_MONTHS = 12;
constexpr uint16_t COLOR_ELECTRICITY = 0x001F;  // blue
constexpr uint16_t COLOR_GAS = 0xFBE0;          // orange/yellow
constexpr uint16_t COLOR_HEADER = 0x001F;
constexpr uint16_t COLOR_SUCCESS = 0x07E0;
constexpr uint16_t COLOR_WARNING = 0xFBE0;
constexpr uint16_t COLOR_GRID = 0xBDF7;
constexpr uint16_t COLOR_MUTED = 0x7BEF;
constexpr uint16_t COLOR_PANEL = 0xFFFF;
constexpr uint8_t PAPER_COLOR_LED_PIN = 21;
constexpr uint8_t PAPER_COLOR_LED_COUNT = 2;

enum Page : int {
  PAGE_WEEK = 0,
  PAGE_MONTH = 1,
  PAGE_YEAR = 2,
  PAGE_COUNT = 3,
};

const char* pageNameC(Page page) {
  switch (page) {
    case PAGE_WEEK: return "Week";
    case PAGE_MONTH: return "Month";
    case PAGE_YEAR: return "Year";
    default: return "Unknown";
  }
}

struct Bucket {
  float electricityKwh = 0;
  float gasKwh = 0;
  float electricityCost = 0;
  float gasCost = 0;
};

struct FuelConfig {
  bool present = false;
  bool isElectricity = false;
  bool smartMeter = true;
  String pointId;
  String serial;
  String productCode;
  String tariffCode;
  float standingChargePence = 0;
};

struct AppConfig {
  bool configured = false;
  String accountNumber;
  String refreshToken;
  FuelConfig electricity;
  FuelConfig gas;
};

struct TokenState {
  String accessToken;
  String refreshToken;
  time_t accessExpiresAt = 0;
  time_t refreshExpiresAt = 0;
};

RTC_DATA_ATTR Bucket rtcWeek[7];
RTC_DATA_ATTR Bucket rtcMonth[MAX_MONTH_DAYS];
RTC_DATA_ATTR Bucket rtcYear[YEAR_MONTHS];
RTC_DATA_ATTR int rtcMonthDays = 0;
RTC_DATA_ATTR int rtcCurrentPage = PAGE_WEEK;
RTC_DATA_ATTR bool rtcHasData = false;
RTC_DATA_ATTR time_t rtcLastRefresh = 0;
RTC_DATA_ATTR char rtcLastError[96] = "";
RTC_DATA_ATTR char rtcWifiDetail[160] = "";

Preferences prefs;
WebServer server(80);
M5Canvas canvas(&M5.Display);
M5PM1 pm1;
Adafruit_NeoPixel statusPixels(PAPER_COLOR_LED_COUNT, PAPER_COLOR_LED_PIN, NEO_GRB + NEO_KHZ800);
AppConfig config;
TokenState tokenState;
bool setupMode = false;
bool webStarted = false;
unsigned long awakeSinceMs = 0;
bool directLedReady = false;
bool m5LedReady = false;
int qrDrawX = 0;
int qrDrawY = 0;
int qrDrawMaxSize = 0;

void logLine(const char* level, const char* func, const char* fmt, ...) {
  char message[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  Serial.printf("[%10lu] [%s] [%s] %s\n", millis(), level, func, message);
}

void ledOff() {
  LOGI("LED off direct=%d m5=%d", directLedReady, m5LedReady);
  if (directLedReady) {
    statusPixels.clear();
    statusPixels.show();
  }
  M5.Led.setBrightness(0);
  M5.Led.display();
  M5.Power.setLed(0);
}

void ledSet(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness = 60) {
  static uint32_t lastLogMs = 0;
  if (millis() - lastLogMs > 1500) {
    LOGI("LED set rgb=(%u,%u,%u) brightness=%u direct=%d m5=%d",
         red, green, blue, brightness, directLedReady, m5LedReady);
    lastLogMs = millis();
  }
  M5.Power.setLed(brightness);
  if (directLedReady) {
    uint8_t r = (static_cast<uint16_t>(red) * brightness) / 255;
    uint8_t g = (static_cast<uint16_t>(green) * brightness) / 255;
    uint8_t b = (static_cast<uint16_t>(blue) * brightness) / 255;
    for (uint8_t i = 0; i < PAPER_COLOR_LED_COUNT; ++i) {
      statusPixels.setPixelColor(i, statusPixels.Color(r, g, b));
    }
    statusPixels.show();
  }
  if (m5LedReady) {
    M5.Led.setBrightness(brightness);
    M5.Led.setAllColor(red, green, blue);
    M5.Led.display();
  }
}

void ledPulse(uint8_t red, uint8_t green, uint8_t blue, int count = 1) {
  LOGI("LED pulse rgb=(%u,%u,%u) count=%d", red, green, blue, count);
  for (int i = 0; i < count; ++i) {
    ledSet(red, green, blue, 90);
    delay(120);
    ledOff();
    delay(120);
  }
}

void initPaperColorHardware() {
  LOGI("initialising Paper Color PM1 hardware");
  if (pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) != M5PM1_OK) {
    LOGE("M5PM1 init failed; display power may be unavailable");
    return;
  }

  pm1.setI2cConfig(0);
  pm1.pinMode(M5PM1_GPIO_NUM_4, OUTPUT);      // SD_DET_EN, matches official demo
  pm1.digitalWrite(M5PM1_GPIO_NUM_4, HIGH);
  pm1.pinMode(M5PM1_GPIO_NUM_1, INPUT_PULLUP);  // SD detect
  pm1.pinMode(M5PM1_GPIO_NUM_0, OUTPUT);      // EPD_EN / PY_EPD_EN
  pm1.digitalWrite(M5PM1_GPIO_NUM_0, HIGH);
  pm1.setChargeEnable(true);
  pm1.setBoostEnable(true);
  LOGI("PM1 ready: EPD power enabled, boost enabled, charger enabled");
  statusPixels.begin();
  statusPixels.setBrightness(255);
  statusPixels.clear();
  statusPixels.show();
  directLedReady = true;
  m5LedReady = M5.Led.begin();
  LOGI("LED init direct_pin=%u direct_count=%u direct_ready=%d m5_enabled=%d m5_begin=%d m5_count=%u",
       PAPER_COLOR_LED_PIN, PAPER_COLOR_LED_COUNT, directLedReady,
       M5.Led.isEnabled(), m5LedReady, M5.Led.getCount());
  delay(50);
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "ssid not available";
    case WL_SCAN_COMPLETED: return "scan completed";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

bool parseHexByte(const char* value, uint8_t* out) {
  if (!isxdigit(value[0]) || !isxdigit(value[1])) return false;
  char buf[3] = {value[0], value[1], '\0'};
  char* end = nullptr;
  long parsed = strtol(buf, &end, 16);
  if (end == buf || parsed < 0 || parsed > 255) return false;
  *out = static_cast<uint8_t>(parsed);
  return true;
}

bool parseBssid(const char* value, uint8_t out[6]) {
  if (value == nullptr || value[0] == '\0') return false;
  for (int i = 0; i < 6; ++i) {
    if (!parseHexByte(value + (i * 3), &out[i])) return false;
    if (i < 5 && value[(i * 3) + 2] != ':') return false;
  }
  return value[17] == '\0';
}

String formatBssid(const uint8_t* bssid) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  return String(buf);
}

String formatAsciiHex(const char* value) {
  String out;
  if (value == nullptr) return out;
  for (size_t i = 0; value[i] != '\0'; ++i) {
    if (i > 0) out += ' ';
    char byte[3];
    snprintf(byte, sizeof(byte), "%02x", static_cast<uint8_t>(value[i]));
    out += byte;
  }
  return out;
}

void renderQrToCanvas(esp_qrcode_handle_t qrcode) {
  int modules = esp_qrcode_get_size(qrcode);
  if (modules <= 0 || qrDrawMaxSize <= 0) return;

  constexpr int quietModules = 4;
  int scale = max(2, qrDrawMaxSize / (modules + quietModules * 2));
  int totalSize = (modules + quietModules * 2) * scale;
  int originX = qrDrawX + max(0, (qrDrawMaxSize - totalSize) / 2);
  int originY = qrDrawY;

  canvas.fillRect(originX, originY, totalSize, totalSize, WHITE);
  for (int y = 0; y < modules; ++y) {
    for (int x = 0; x < modules; ++x) {
      if (esp_qrcode_get_module(qrcode, x, y)) {
        canvas.fillRect(originX + (x + quietModules) * scale,
                        originY + (y + quietModules) * scale,
                        scale, scale, BLACK);
      }
    }
  }
  canvas.drawRect(originX - 1, originY - 1, totalSize + 2, totalSize + 2, BLACK);
}

bool drawQrCode(const String& url, int x, int y, int maxSize) {
  qrDrawX = x;
  qrDrawY = y;
  qrDrawMaxSize = maxSize;

  esp_qrcode_config_t qrConfig = ESP_QRCODE_CONFIG_DEFAULT();
  qrConfig.display_func = renderQrToCanvas;
  qrConfig.max_qrcode_version = 6;
  qrConfig.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
  esp_err_t err = esp_qrcode_generate(&qrConfig, url.c_str());
  LOGI("drawQrCode url=%s max_size=%d result=0x%x", url.c_str(), maxSize, err);
  qrDrawMaxSize = 0;
  return err == ESP_OK;
}

void setWifiDetail(const String& detail) {
  strlcpy(rtcWifiDetail, detail.c_str(), sizeof(rtcWifiDetail));
  LOGI("wifi detail: %s", detail.c_str());
}

String htmlEscape(const String& input) {
  String out;
  out.reserve(input.length());
  for (char c : input) {
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      default: out += c; break;
    }
  }
  return out;
}

String ellipsizeText(const String& text, int maxWidth) {
  if (canvas.textWidth(text) <= maxWidth) return text;
  String out = text;
  while (out.length() > 0 && canvas.textWidth(out + "...") > maxWidth) {
    out.remove(out.length() - 1);
  }
  return out + "...";
}

void drawClippedString(const String& text, int x, int y, int maxWidth) {
  canvas.drawString(ellipsizeText(text, maxWidth), x, y);
}

bool hasSetupHeaderCredentials() {
  return strlen(EDF_EMAIL) > 0 && strlen(EDF_PASSWORD) > 0 && strlen(EDF_ACCOUNT) > 0;
}

void setLastError(const String& message) {
  strlcpy(rtcLastError, message.c_str(), sizeof(rtcLastError));
  LOGE("%s", message.c_str());
}

String graphqlErrorSummary(JsonDocument& response) {
  JsonArray errors = response["errors"].as<JsonArray>();
  if (errors.isNull() || errors.size() == 0) return "";

  String summary;
  int count = 0;
  for (JsonObject error : errors) {
    if (count > 0) summary += " | ";
    const char* message = error["message"] | "GraphQL error";
    const char* code = error["extensions"]["errorCode"] | nullptr;
    summary += message;
    if (code != nullptr) {
      summary += " [";
      summary += code;
      summary += "]";
    }
    if (++count >= 3) break;
  }
  if (errors.size() > static_cast<size_t>(count)) {
    summary += " | +";
    summary += String(errors.size() - count);
    summary += " more";
  }
  return summary;
}

void logGraphqlResponseShape(JsonDocument& response, const char* context) {
  bool hasData = !response["data"].isNull();
  bool hasErrors = !response["errors"].isNull();
  bool hasTokenNode = !response["data"]["obtainKrakenToken"].isNull();
  bool hasAccess = !response["data"]["obtainKrakenToken"]["token"].isNull();
  bool hasRefresh = !response["data"]["obtainKrakenToken"]["refreshToken"].isNull();
  LOGI("GraphQL response shape context=%s has_data=%d has_errors=%d token_node=%d access=%d refresh=%d",
       context, hasData, hasErrors, hasTokenNode, hasAccess, hasRefresh);
  String errors = graphqlErrorSummary(response);
  if (errors.length() > 0) {
    LOGW("GraphQL errors context=%s summary=%s", context, errors.c_str());
  }
}

void clearLastError() {
  if (rtcLastError[0] != '\0') LOGI("clearing last error: %s", rtcLastError);
  rtcLastError[0] = '\0';
}

time_t nowUtc() {
  return time(nullptr);
}

String isoUtc(time_t t) {
  struct tm tm {};
  gmtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return String(buf);
}

String dateLabel(time_t t, const char* fmt) {
  struct tm tm {};
  localtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), fmt, &tm);
  return String(buf);
}

time_t localTimeAt(int year, int mon, int mday, int hour = 0, int min = 0, int sec = 0) {
  struct tm tm {};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = mday;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = sec;
  tm.tm_isdst = -1;
  return mktime(&tm);
}

time_t startOfTodayLocal() {
  time_t n = nowUtc();
  struct tm tm {};
  localtime_r(&n, &tm);
  return localTimeAt(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

time_t startOfWeekLocal() {
  return startOfTodayLocal() - 6 * 86400;
}

time_t startOfMonthLocal() {
  return startOfTodayLocal() - 29 * 86400;
}

time_t startOfYearLocal() {
  time_t n = nowUtc();
  struct tm tm {};
  localtime_r(&n, &tm);
  return localTimeAt(tm.tm_year + 1900, tm.tm_mon + 1 - 11, 1);
}

int daysInCurrentMonth() {
  return 30;
}

int bucketIndexFor(time_t t, Page page, time_t pageStart) {
  struct tm item {};
  struct tm start {};
  localtime_r(&t, &item);
  localtime_r(&pageStart, &start);

  if (page == PAGE_WEEK || page == PAGE_MONTH) {
    time_t dayStart = localTimeAt(item.tm_year + 1900, item.tm_mon + 1, item.tm_mday);
    return static_cast<int>((dayStart - pageStart) / 86400);
  }

  int startYear = start.tm_year;
  int startMon = start.tm_mon;
  int itemYear = item.tm_year;
  int itemMon = item.tm_mon;
  return (itemYear - startYear) * 12 + (itemMon - startMon);
}

float convertGasM3ToKwh(float m3) {
  // Same common conversion shape used by energy suppliers:
  // m3 * correction factor * calorific value / kWh conversion factor.
  constexpr float correction = 1.02264f;
  constexpr float calorific = 39.5f;
  constexpr float kwhFactor = 3.6f;
  return m3 * correction * calorific / kwhFactor;
}

bool parseIsoTime(const char* value, time_t* out) {
  if (value == nullptr || strlen(value) < 19) return false;

  int year, mon, day, hour, min, sec;
  if (sscanf(value, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) != 6) {
    return false;
  }

  struct tm tm {};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = sec;

  // Kraken API timestamps are UTC for the endpoints this app uses. The ESP32
  // Arduino toolchain does not expose timegm(), so temporarily evaluate mktime
  // under UTC and then restore the display timezone.
  setenv("TZ", "UTC0", 1);
  tzset();
  *out = mktime(&tm);
  setenv("TZ", TZ_LONDON, 1);
  tzset();
  return true;
}

void saveConfig() {
  LOGI("saving config: configured=%d account=%s elec=%d gas=%d refresh_token_present=%d",
       config.configured, config.accountNumber.c_str(), config.electricity.present,
       config.gas.present, config.refreshToken.length() > 0);
  prefs.begin("edf", false);
  prefs.putBool("configured", config.configured);
  prefs.putString("account", config.accountNumber);
  prefs.putString("refresh", config.refreshToken);
  prefs.putBool("elecPresent", config.electricity.present);
  prefs.putString("elecMpan", config.electricity.pointId);
  prefs.putString("elecSerial", config.electricity.serial);
  prefs.putString("elecProduct", config.electricity.productCode);
  prefs.putString("elecTariff", config.electricity.tariffCode);
  prefs.putBool("elecSmart", config.electricity.smartMeter);
  prefs.putBool("gasPresent", config.gas.present);
  prefs.putString("gasMprn", config.gas.pointId);
  prefs.putString("gasSerial", config.gas.serial);
  prefs.putString("gasProduct", config.gas.productCode);
  prefs.putString("gasTariff", config.gas.tariffCode);
  prefs.end();
}

void loadConfig() {
  LOGI("loading config from NVS");
  prefs.begin("edf", true);
  config.configured = prefs.getBool("configured", false);
  config.accountNumber = prefs.getString("account", "");
  config.refreshToken = prefs.getString("refresh", "");

  config.electricity = FuelConfig {};
  config.electricity.present = prefs.getBool("elecPresent", false);
  config.electricity.isElectricity = true;
  config.electricity.pointId = prefs.getString("elecMpan", "");
  config.electricity.serial = prefs.getString("elecSerial", "");
  config.electricity.productCode = prefs.getString("elecProduct", "");
  config.electricity.tariffCode = prefs.getString("elecTariff", "");
  config.electricity.smartMeter = prefs.getBool("elecSmart", true);

  config.gas = FuelConfig {};
  config.gas.present = prefs.getBool("gasPresent", false);
  config.gas.isElectricity = false;
  config.gas.pointId = prefs.getString("gasMprn", "");
  config.gas.serial = prefs.getString("gasSerial", "");
  config.gas.productCode = prefs.getString("gasProduct", "");
  config.gas.tariffCode = prefs.getString("gasTariff", "");
  prefs.end();
  LOGI("loaded config: configured=%d account=%s elec=%d gas=%d refresh_token_present=%d",
       config.configured, config.accountNumber.c_str(), config.electricity.present,
       config.gas.present, config.refreshToken.length() > 0);
}

void clearConfig() {
  LOGW("clearing stored EDF config and cached RTC data");
  prefs.begin("edf", false);
  prefs.clear();
  prefs.end();
  config = AppConfig {};
  rtcHasData = false;
}

bool postJson(const String& url, const String& payload, JsonDocument& response, const String& auth = "", const String& context = "papercolor") {
  LOGI("HTTP POST start context=%s url=%s payload_bytes=%u auth_present=%d",
       context.c_str(), url.c_str(), payload.length(), auth.length() > 0);
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
    setLastError("JSON parse failed");
    return false;
  }
  LOGI("HTTP POST parsed context=%s", context.c_str());
  return true;
}

bool getJson(const String& url, JsonDocument& response, const String& auth = "", const String& context = "papercolor") {
  LOGI("HTTP GET start context=%s url=%s auth_present=%d", context.c_str(), url.c_str(), auth.length() > 0);
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
  LOGI("HTTP GET complete context=%s status=%d", context.c_str(), code);
  if (code < 200 || code >= 300) {
    ledPulse(255, 0, 0, 2);
    setLastError("HTTP GET failed: " + String(code));
    http.end();
    return false;
  }

  DeserializationError err = deserializeJson(response, http.getStream());
  http.end();
  if (err) {
    ledPulse(255, 0, 0, 2);
    setLastError("JSON parse failed");
    return false;
  }
  LOGI("HTTP GET parsed context=%s", context.c_str());
  return true;
}

bool obtainRefreshToken(const String& email, const String& password, String* refreshToken) {
  LOGI("obtaining initial refresh token for email_present=%d password_present=%d",
       email.length() > 0, password.length() > 0);
  JsonDocument payload;
  payload["query"] =
    "mutation ObtainToken($email: String!, $password: String!) {"
    "  obtainKrakenToken(input: { email: $email, password: $password }) {"
    "    token refreshToken refreshExpiresIn"
    "  }"
    "}";
  payload["variables"]["email"] = email;
  payload["variables"]["password"] = password;

  String body;
  serializeJson(payload, body);

  JsonDocument response;
  if (!postJson(GRAPHQL_URL, body, response, "", "initial-token")) return false;
  logGraphqlResponseShape(response, "initial-token");

  String token = response["data"]["obtainKrakenToken"]["refreshToken"].as<String>();
  LOGI("initial token fields access_len=%u refresh_len=%u",
       response["data"]["obtainKrakenToken"]["token"].as<String>().length(), token.length());
  if (token.length() == 0) {
    String errors = graphqlErrorSummary(response);
    setLastError(errors.length() > 0 ? ("EDF login failed: " + errors) : "EDF login failed: no refresh token");
    return false;
  }

  *refreshToken = token;
  LOGI("initial refresh token obtained");
  return true;
}

bool refreshAccessToken() {
  LOGI("refreshAccessToken start access_present=%d refresh_present=%d access_expires_in=%ld",
       tokenState.accessToken.length() > 0,
       (tokenState.refreshToken.length() > 0 || config.refreshToken.length() > 0),
       static_cast<long>(tokenState.accessExpiresAt - nowUtc()));
  if (tokenState.accessToken.length() > 0 && tokenState.accessExpiresAt - 300 > nowUtc()) {
    LOGI("using cached access token");
    return true;
  }

  String refreshToken = tokenState.refreshToken.length() > 0 ? tokenState.refreshToken : config.refreshToken;
  if (refreshToken.length() == 0) {
    setLastError("Missing refresh token");
    return false;
  }

  JsonDocument payload;
  payload["query"] =
    "mutation RefreshToken($refreshToken: String!) {"
    "  obtainKrakenToken(input: { refreshToken: $refreshToken }) {"
    "    token refreshToken refreshExpiresIn"
    "  }"
    "}";
  payload["variables"]["refreshToken"] = refreshToken;

  String body;
  serializeJson(payload, body);

  JsonDocument response;
  if (!postJson(GRAPHQL_URL, body, response, "", "refresh-token")) return false;
  logGraphqlResponseShape(response, "refresh-token");

  JsonObject token = response["data"]["obtainKrakenToken"];
  String access = token["token"].as<String>();
  String rotated = token["refreshToken"].as<String>();
  LOGI("refresh token fields access_len=%u refresh_len=%u",
       access.length(), rotated.length());
  if (access.length() == 0 || rotated.length() == 0) {
    String errors = graphqlErrorSummary(response);
    setLastError(errors.length() > 0 ? ("Token refresh failed: " + errors) : "Token refresh failed: missing token fields");
    return false;
  }

  tokenState.accessToken = access;
  tokenState.refreshToken = rotated;
  tokenState.accessExpiresAt = nowUtc() + 3600;
  tokenState.refreshExpiresAt = token["refreshExpiresIn"] | 0;
  LOGI("access token refreshed refresh_expires_at=%ld rotated=%d",
       static_cast<long>(tokenState.refreshExpiresAt), config.refreshToken != tokenState.refreshToken);

  if (config.refreshToken != tokenState.refreshToken) {
    LOGI("refresh token rotated; saving updated token");
    config.refreshToken = tokenState.refreshToken;
    saveConfig();
  }
  return true;
}

String authHeader(bool jwtPrefix = true) {
  return jwtPrefix ? "JWT " + tokenState.accessToken : tokenState.accessToken;
}

bool discoverAccount(const String& accountNumber) {
  LOGI("discovering account account=%s", accountNumber.c_str());
  if (!refreshAccessToken()) return false;

  JsonDocument payload;
  String query =
    "query {"
    "  account(accountNumber: \"" + accountNumber + "\") {"
    "    electricityAgreements(active: true) {"
    "      meterPoint {"
    "        mpan"
    "        meters(includeInactive: false) { serialNumber smartImportElectricityMeter { deviceId } }"
    "        agreements(includeInactive: true) { validFrom validTo tariff { ... on TariffType { productCode tariffCode } } }"
    "      }"
    "    }"
    "    gasAgreements(active: true) {"
    "      meterPoint {"
    "        mprn"
    "        meters(includeInactive: false) { serialNumber consumptionUnits smartGasMeter { deviceId } }"
    "        agreements(includeInactive: true) { validFrom validTo tariff { productCode tariffCode } }"
    "      }"
    "    }"
    "  }"
    "}";
  payload["query"] = query;

  String body;
  serializeJson(payload, body);

  JsonDocument response;
  if (!postJson(GRAPHQL_URL, body, response, authHeader(false), "get-account")) return false;

  JsonObject account = response["data"]["account"];
  if (account.isNull()) {
    setLastError("Account not found");
    return false;
  }

  config.electricity = FuelConfig {};
  config.electricity.isElectricity = true;
  for (JsonObject agreement : account["electricityAgreements"].as<JsonArray>()) {
    JsonObject meterPoint = agreement["meterPoint"];
    JsonObject meter = meterPoint["meters"][0];
    JsonObject tariffAgreement = meterPoint["agreements"][0];
    const char* product = tariffAgreement["tariff"]["productCode"] | "";
    const char* tariff = tariffAgreement["tariff"]["tariffCode"] | "";
    if (meterPoint["mpan"].is<const char*>() && meter["serialNumber"].is<const char*>() && strlen(product) > 0 && strlen(tariff) > 0) {
      config.electricity.present = true;
      config.electricity.pointId = meterPoint["mpan"].as<const char*>();
      config.electricity.serial = meter["serialNumber"].as<const char*>();
      config.electricity.smartMeter = !meter["smartImportElectricityMeter"].isNull();
      config.electricity.productCode = product;
      config.electricity.tariffCode = tariff;
      LOGI("discovered electricity mpan=%s serial=%s product=%s tariff=%s smart=%d",
           config.electricity.pointId.c_str(), config.electricity.serial.c_str(),
           config.electricity.productCode.c_str(), config.electricity.tariffCode.c_str(),
           config.electricity.smartMeter);
      break;
    }
  }

  config.gas = FuelConfig {};
  config.gas.isElectricity = false;
  for (JsonObject agreement : account["gasAgreements"].as<JsonArray>()) {
    JsonObject meterPoint = agreement["meterPoint"];
    JsonObject meter = meterPoint["meters"][0];
    JsonObject tariffAgreement = meterPoint["agreements"][0];
    const char* product = tariffAgreement["tariff"]["productCode"] | "";
    const char* tariff = tariffAgreement["tariff"]["tariffCode"] | "";
    if (meterPoint["mprn"].is<const char*>() && meter["serialNumber"].is<const char*>() && strlen(product) > 0 && strlen(tariff) > 0) {
      config.gas.present = true;
      config.gas.pointId = meterPoint["mprn"].as<const char*>();
      config.gas.serial = meter["serialNumber"].as<const char*>();
      config.gas.productCode = product;
      config.gas.tariffCode = tariff;
      LOGI("discovered gas mprn=%s serial=%s product=%s tariff=%s",
           config.gas.pointId.c_str(), config.gas.serial.c_str(),
           config.gas.productCode.c_str(), config.gas.tariffCode.c_str());
      break;
    }
  }

  if (!config.electricity.present && !config.gas.present) {
    setLastError("No meters discovered");
    return false;
  }

  config.accountNumber = accountNumber;
  config.configured = true;
  saveConfig();
  LOGI("account discovery complete elec=%d gas=%d", config.electricity.present, config.gas.present);
  return true;
}

float rateAt(JsonArray rates, time_t slotStart) {
  for (JsonObject rate : rates) {
    time_t start = 0, end = 0;
    const char* startText = rate["valid_from"];
    if (!startText) startText = rate["start"];
    const char* endText = rate["valid_to"];
    if (!endText) endText = rate["end"];
    if (!parseIsoTime(startText, &start)) continue;
    bool hasEnd = parseIsoTime(endText, &end);
    if (slotStart >= start && (!hasEnd || slotStart < end)) {
      return rate["value_inc_vat"] | 0.0f;
    }
  }
  return 0;
}

bool fetchRates(const FuelConfig& fuel, time_t from, time_t to, JsonDocument& ratesOut) {
  LOGI("fetchRates fuel=%s present=%d point=%s product=%s tariff=%s from=%s to=%s",
       fuel.isElectricity ? "electricity" : "gas", fuel.present, fuel.pointId.c_str(),
       fuel.productCode.c_str(), fuel.tariffCode.c_str(), isoUtc(from).c_str(), isoUtc(to).c_str());
  if (fuel.productCode.length() == 0 || fuel.tariffCode.length() == 0) return false;

  String url;
  if (fuel.isElectricity) {
    url = String(EDF_BASE_URL) + "/v1/products/" + fuel.productCode +
          "/electricity-tariffs/" + fuel.tariffCode +
          "/standard-unit-rates/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=1500";
  } else {
    url = String(EDF_BASE_URL) + "/v1/products/" + fuel.productCode +
          "/gas-tariffs/" + fuel.tariffCode +
          "/standard-unit-rates/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=1500";
  }

  if (!getJson(url, ratesOut, authHeader(), fuel.isElectricity ? "electricity-rates" : "gas-rates")) {
    return false;
  }
  LOGI("fetchRates complete fuel=%s result_count=%u",
       fuel.isElectricity ? "electricity" : "gas", ratesOut["results"].as<JsonArray>().size());
  return true;
}

bool fetchStandingCharge(const FuelConfig& fuel, time_t from, time_t to, float* standingChargePence) {
  LOGI("fetchStandingCharge fuel=%s from=%s to=%s",
       fuel.isElectricity ? "electricity" : "gas", isoUtc(from).c_str(), isoUtc(to).c_str());
  if (fuel.productCode.length() == 0 || fuel.tariffCode.length() == 0) return false;

  String url;
  if (fuel.isElectricity) {
    url = String(EDF_BASE_URL) + "/v1/products/" + fuel.productCode +
          "/electricity-tariffs/" + fuel.tariffCode +
          "/standing-charges/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=50";
  } else {
    url = String(EDF_BASE_URL) + "/v1/products/" + fuel.productCode +
          "/gas-tariffs/" + fuel.tariffCode +
          "/standing-charges/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=50";
  }

  JsonDocument doc;
  if (!getJson(url, doc, authHeader(), fuel.isElectricity ? "electricity-standing-charge" : "gas-standing-charge")) {
    return false;
  }

  JsonArray results = doc["results"].as<JsonArray>();
  if (results.size() == 0) {
    *standingChargePence = 0;
    return true;
  }

  *standingChargePence = results[0]["value_inc_vat"] | 0.0f;
  LOGI("standing charge fuel=%s pence=%0.3f", fuel.isElectricity ? "electricity" : "gas", *standingChargePence);
  return true;
}

void addToBucket(Bucket& bucket, bool electricity, float kwh, float costPounds) {
  if (electricity) {
    bucket.electricityKwh += kwh;
    bucket.electricityCost += costPounds;
  } else {
    bucket.gasKwh += kwh;
    bucket.gasCost += costPounds;
  }
}

bool fetchFuelWindow(const FuelConfig& fuel, Page page, time_t from, time_t to, Bucket* buckets, int bucketCount, time_t pageStart = 0) {
  LOGI("fetchFuelWindow start fuel=%s present=%d page=%s from=%s to=%s bucket_count=%d",
       fuel.isElectricity ? "electricity" : "gas", fuel.present,
       pageNameC(page), isoUtc(from).c_str(), isoUtc(to).c_str(), bucketCount);
  if (!fuel.present) return true;
  if (!refreshAccessToken()) return false;

  JsonDocument ratesDoc;
  if (!fetchRates(fuel, from, to, ratesDoc)) return false;
  JsonArray rates = ratesDoc["results"].as<JsonArray>();

  float standingChargePence = 0;
  fetchStandingCharge(fuel, from, to, &standingChargePence);

  String url;
  if (fuel.isElectricity) {
    url = String(EDF_BASE_URL) + "/v1/electricity-meter-points/" + fuel.pointId +
          "/meters/" + fuel.serial + "/consumption/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=1500";
  } else {
    url = String(EDF_BASE_URL) + "/v1/gas-meter-points/" + fuel.pointId +
          "/meters/" + fuel.serial + "/consumption/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=1500";
  }

  while (url.length() > 0) {
    JsonDocument doc;
    if (!getJson(url, doc, authHeader(), fuel.isElectricity ? "electricity-consumption" : "gas-consumption")) {
      return false;
    }

    JsonArray results = doc["results"].as<JsonArray>();
    LOGI("fetchFuelWindow parsed data: %d items", results.size());

    int pageItems = 0;
    for (JsonObject item : results) {
      const char* startText = item["interval_start"];
      if (!startText) startText = item["start"];
      time_t slotStart = 0;
      if (!parseIsoTime(startText, &slotStart)) continue;
      time_t pStart = pageStart != 0 ? pageStart : from;
      int index = bucketIndexFor(slotStart, page, pStart);
      if (index < 0 || index >= bucketCount) continue;

      float consumption = item["consumption"] | 0.0f;
      float kwh = fuel.isElectricity ? consumption : convertGasM3ToKwh(consumption);
      float pencePerKwh = rateAt(rates, slotStart);
      float costPounds = (kwh * pencePerKwh) / 100.0f;
      costPounds += (standingChargePence / 48.0f) / 100.0f;
      addToBucket(buckets[index], fuel.isElectricity, kwh, costPounds);
      pageItems++;
    }

    const char* next = doc["next"] | nullptr;
    LOGI("fetchFuelWindow page fuel=%s items=%d has_next=%d",
         fuel.isElectricity ? "electricity" : "gas", pageItems, next != nullptr);
    url = next == nullptr ? "" : String(next);
  }

  LOGI("fetchFuelWindow complete fuel=%s page=%s", fuel.isElectricity ? "electricity" : "gas", pageNameC(page));
  return true;
}

void resetBuckets(Bucket* buckets, int count) {
  for (int i = 0; i < count; ++i) buckets[i] = Bucket {};
}

bool refreshData() {
  LOGI("refreshData start");
  ledPulse(0, 255, 255, 2);
  clearLastError();
  
  if (rtcCurrentPage == PAGE_WEEK) {
    resetBuckets(rtcWeek, 7);
  } else if (rtcCurrentPage == PAGE_MONTH) {
    resetBuckets(rtcMonth, MAX_MONTH_DAYS);
  } else if (rtcCurrentPage == PAGE_YEAR) {
    resetBuckets(rtcYear, YEAR_MONTHS);
  }
  
  rtcMonthDays = daysInCurrentMonth();

  time_t weekStart = startOfWeekLocal();
  time_t monthStart = startOfMonthLocal();
  time_t yearStart = startOfYearLocal();
  time_t nextWeek = weekStart + 7 * 86400;
  time_t nextMonth = monthStart + 30 * 86400;
  time_t nextYear;
  {
    struct tm tm {};
    localtime_r(&yearStart, &tm);
    nextYear = localTimeAt(tm.tm_year + 1900, tm.tm_mon + 1 + 12, 1);
  }
  LOGI("refresh windows week=%s..%s month=%s..%s year=%s..%s month_days=%d",
       isoUtc(weekStart).c_str(), isoUtc(nextWeek).c_str(),
       isoUtc(monthStart).c_str(), isoUtc(nextMonth).c_str(),
       isoUtc(yearStart).c_str(), isoUtc(nextYear).c_str(), rtcMonthDays);

  if (rtcCurrentPage == PAGE_WEEK) {
    if (!fetchFuelWindow(config.electricity, PAGE_WEEK, weekStart, nextWeek, rtcWeek, 7)) { rtcHasData = false; return false; }
    if (!fetchFuelWindow(config.gas, PAGE_WEEK, weekStart, nextWeek, rtcWeek, 7)) { rtcHasData = false; return false; }
  } else if (rtcCurrentPage == PAGE_MONTH) {
    if (!fetchFuelWindow(config.electricity, PAGE_MONTH, monthStart, nextMonth, rtcMonth, rtcMonthDays)) { rtcHasData = false; return false; }
    if (!fetchFuelWindow(config.gas, PAGE_MONTH, monthStart, nextMonth, rtcMonth, rtcMonthDays)) { rtcHasData = false; return false; }
  } else if (rtcCurrentPage == PAGE_YEAR) {
    // Fetch the yearly view one month at a time. This keeps JSON documents small and avoids
    // holding a full year of half-hour slots in memory.
    for (int month = 0; month < YEAR_MONTHS; ++month) {
      struct tm tm {};
      localtime_r(&yearStart, &tm);
      time_t from = localTimeAt(tm.tm_year + 1900, tm.tm_mon + 1 + month, 1);
      time_t to = localTimeAt(tm.tm_year + 1900, tm.tm_mon + 2 + month, 1);
      if (!fetchFuelWindow(config.electricity, PAGE_YEAR, from, to, rtcYear, YEAR_MONTHS, yearStart)) { rtcHasData = false; return false; }
      if (!fetchFuelWindow(config.gas, PAGE_YEAR, from, to, rtcYear, YEAR_MONTHS, yearStart)) { rtcHasData = false; return false; }
    }
  }

  rtcHasData = true;
  rtcLastRefresh = nowUtc();
  clearLastError();
  ledPulse(0, 255, 0, 2);
  LOGI("refreshData complete last_refresh=%ld", static_cast<long>(rtcLastRefresh));
  return true;
}

String pageName(Page page) {
  return String(pageNameC(page));
}

float bucketTotal(const Bucket& bucket, bool cost) {
  return cost ? bucket.electricityCost + bucket.gasCost : bucket.electricityKwh + bucket.gasKwh;
}

void drawStackedChart(int x, int y, int w, int h, const String& title, Bucket* buckets, int count, bool cost) {
  LOGI("drawStackedChart title=%s x=%d y=%d w=%d h=%d buckets=%d metric=%s",
       title.c_str(), x, y, w, h, count, cost ? "cost" : "energy");
  canvas.setTextColor(BLACK);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextDatum(top_left);
  canvas.drawString(title, x, y);

  int chartY = y + 22;
  int chartH = h - 44;
  canvas.fillRect(x, chartY, w, chartH, COLOR_PANEL);
  canvas.drawRect(x, chartY, w, chartH, BLACK);
  for (int i = 1; i < 4; ++i) {
    int gy = chartY + (chartH * i) / 4;
    canvas.drawFastHLine(x + 1, gy, w - 2, COLOR_GRID);
  }

  float maxTotal = 0;
  for (int i = 0; i < count; ++i) {
    maxTotal = max(maxTotal, bucketTotal(buckets[i], cost));
  }
  if (maxTotal <= 0.001f) maxTotal = 1;

  int gap = count > 12 ? 2 : 5;
  int barW = max(2, (w - 10 - (count - 1) * gap) / count);
  int startX = x + 5;
  int baseline = chartY + chartH - 1;

  for (int i = 0; i < count; ++i) {
    float elec = cost ? buckets[i].electricityCost : buckets[i].electricityKwh;
    float gas = cost ? buckets[i].gasCost : buckets[i].gasKwh;
    int elecH = static_cast<int>((elec / maxTotal) * (chartH - 4));
    int gasH = static_cast<int>((gas / maxTotal) * (chartH - 4));
    int bx = startX + i * (barW + gap);
    if (gasH > 0) canvas.fillRect(bx, baseline - gasH, barW, gasH, COLOR_GAS);
    if (elecH > 0) canvas.fillRect(bx, baseline - gasH - elecH, barW, elecH, COLOR_ELECTRICITY);

    float total = elec + gas;
    if (total > 0.01f && barW >= 20) {
      canvas.setFont(&fonts::Font0);
      canvas.setTextColor(BLACK);
      canvas.setTextDatum(bottom_center);
      String label = cost ? String(total, 2) : String(total, 1);
      canvas.drawString(label, bx + barW / 2, baseline - gasH - elecH - 2);
    }
  }

  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(COLOR_MUTED);
  canvas.setTextDatum(top_left);
  String maxLabel = cost ? ("GBP " + String(maxTotal, 2)) : (String(maxTotal, 1) + " kWh");
  drawClippedString(maxLabel, x, chartY + chartH + 3, w / 2);

  canvas.setTextDatum(top_right);
  if (count == 7) {
    canvas.drawString("Last 7 days", x + w, chartY + chartH + 3);
  } else if (count == YEAR_MONTHS) {
    canvas.drawString("Last 12 months", x + w, chartY + chartH + 3);
  } else {
    canvas.drawString("Last " + String(count) + " days", x + w, chartY + chartH + 3);
  }
}

void drawStatusScreen(const String& title, const String& message) {
  LOGI("drawStatusScreen title=%s message=%s wifi=%d error=%s",
       title.c_str(), message.c_str(), WiFi.isConnected(), rtcLastError);
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  int W = M5.Display.width();
  int H = M5.Display.height();
  canvas.fillSprite(WHITE);
  canvas.fillRect(0, 0, W, 52, COLOR_HEADER);
  canvas.setTextColor(WHITE);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextDatum(top_left);
  drawClippedString(title, 18, 14, W - 36);
  canvas.setTextColor(BLACK);
  canvas.setFont(&fonts::FreeSans9pt7b);
  drawClippedString(message, 18, 70, W - 36);
  if (WiFi.isConnected()) {
    String setupUrl = "http://" + WiFi.localIP().toString() + "/";
    canvas.setTextColor(COLOR_HEADER);
    drawClippedString(setupUrl, 18, 102, W - 36);
    if (title == "Setup required") {
      canvas.setFont(&fonts::Font2);
      canvas.setTextColor(COLOR_MUTED);
      canvas.drawString("Scan to configure", 18, 132);
      drawQrCode(setupUrl, 64, 158, min(270, W - 128));
      canvas.setTextColor(BLACK);
    }
  } else if (rtcWifiDetail[0] != '\0') {
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(BLACK);
    drawClippedString(rtcWifiDetail, 18, 102, W - 36);
  }
  if (rtcLastError[0] != '\0') {
    int errorY = H - 82;
    canvas.fillRect(0, errorY - 8, W, 50, WHITE);
    canvas.drawFastHLine(18, errorY - 10, W - 36, COLOR_GRID);
    canvas.setTextColor(RED);
    canvas.setFont(&fonts::FreeSans9pt7b);
    drawClippedString(String("Error: ") + rtcLastError, 18, errorY, W - 36);
  }
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(COLOR_MUTED);
  canvas.drawString(String("LED enabled: ") + (M5.Led.isEnabled() ? "yes" : "no"), 18, H - 26);
  canvas.pushSprite(0, 0);
}

void drawDashboard() {
  LOGI("drawDashboard page=%s has_data=%d last_refresh=%ld wifi=%d",
       pageNameC(static_cast<Page>(rtcCurrentPage)), rtcHasData,
       static_cast<long>(rtcLastRefresh), WiFi.isConnected());
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  canvas.fillSprite(WHITE);
  int W = M5.Display.width();
  int H = M5.Display.height();
  Page page = static_cast<Page>(rtcCurrentPage);

  canvas.fillRect(0, 0, W, 52, COLOR_HEADER);
  canvas.setTextColor(WHITE);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextDatum(middle_left);
  drawClippedString("EDF " + pageName(page), 14, 26, W - 180);

  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(top_left);
  String status = WiFi.isConnected() ? WiFi.localIP().toString() : "offline";
  drawClippedString(status, W - 150, 8, 138);
  String refreshed = rtcLastRefresh > 0 ? dateLabel(rtcLastRefresh, "%d %b %H:%M") : "not refreshed";
  drawClippedString(refreshed, W - 150, 28, 138);

  Bucket* buckets = rtcWeek;
  int count = 7;
  if (page == PAGE_MONTH) {
    buckets = rtcMonth;
    count = max(1, rtcMonthDays);
  } else if (page == PAGE_YEAR) {
    buckets = rtcYear;
    count = YEAR_MONTHS;
  }

  int chartX = 14;
  int chartW = W - 28;
  int footerY = H - 32;
  int legendY = footerY - 18;
  int chartTop = 64;
  int chartGap = 18;
  int chartBottom = legendY - 8;
  int chartH = max(120, (chartBottom - chartTop - chartGap) / 2);
  drawStackedChart(chartX, chartTop, chartW, chartH, "Cost", buckets, count, true);
  drawStackedChart(chartX, chartTop + chartH + chartGap, chartW, chartH, "Energy", buckets, count, false);

  canvas.drawFastHLine(0, footerY, W, BLACK);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(BLACK);
  canvas.setTextDatum(middle_left);
  canvas.fillRect(12, legendY + 2, 10, 10, COLOR_ELECTRICITY);
  canvas.drawString("Electric", 28, legendY + 7);
  canvas.fillRect(96, legendY + 2, 10, 10, COLOR_GAS);
  canvas.drawString("Gas", 112, legendY + 7);
  canvas.setTextDatum(middle_right);
  canvas.drawString("G10 Up  G9 Down  G1 Refresh", W - 10, footerY + 15);

  if (rtcLastError[0] != '\0') {
    canvas.setTextColor(RED);
    canvas.setTextDatum(top_left);
    drawClippedString(rtcLastError, 14, 54, W - 28);
  }

  canvas.pushSprite(0, 0);
}

bool connectWifi() {
  rtcWifiDetail[0] = '\0';
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(300);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  wifi_country_t country = {};
  country.cc[0] = 'G';
  country.cc[1] = 'B';
  country.cc[2] = 'O';
  country.schan = 1;
  country.nchan = 13;
  country.policy = WIFI_COUNTRY_POLICY_AUTO;
  esp_wifi_set_country(&country);

#if HAS_LOCAL_WIFI_SECRETS
  setWifiDetail("Wi-Fi config: local secrets header loaded");
#else
  setWifiDetail("Wi-Fi config: include/secrets.h missing");
#endif

  ledSet(255, 0, 0, 70);
  uint8_t configuredBssid[6] = {};
  bool hasBssid = parseBssid(WIFI_BSSID, configuredBssid);
  String configuredBssidString = hasBssid ? formatBssid(configuredBssid) : String("");
  LOGI("Wi-Fi target ssid=%s ssid_len=%u ssid_hex=%s password_len=%u hidden=%d configured_channel=%d bssid_set=%d",
       WIFI_SSID, strlen(WIFI_SSID), formatAsciiHex(WIFI_SSID).c_str(),
       strlen(WIFI_PASSWORD), WIFI_HIDDEN, WIFI_CHANNEL, hasBssid);
  if (WIFI_BSSID[0] != '\0' && !hasBssid) {
    LOGW("WIFI_BSSID is set but invalid; expected aa:bb:cc:dd:ee:ff");
  }

  struct HiddenCandidate {
    uint8_t bssid[6];
    int channel;
    int rssi;
    int auth;
  };
  constexpr int MAX_HIDDEN_CANDIDATES = 12;
  HiddenCandidate hiddenCandidates[MAX_HIDDEN_CANDIDATES] = {};
  int hiddenCandidateCount = 0;

  String scanDetail;
  if (WIFI_HIDDEN) {
    scanDetail = String("Hidden SSID mode");
    if (WIFI_CHANNEL > 0) scanDetail += String("; channel ") + WIFI_CHANNEL;
    if (hasBssid) scanDetail += "; BSSID set";

    LOGI("Wi-Fi hidden preflight: active SSID scan start channel=%d bssid_set=%d",
         WIFI_CHANNEL, hasBssid);
    int specificCount = WiFi.scanNetworks(false, true, false, 600, WIFI_CHANNEL, WIFI_SSID,
                                          hasBssid ? configuredBssid : nullptr);
    LOGI("Wi-Fi hidden preflight: active SSID scan complete count=%d", specificCount);
    for (int i = 0; i < specificCount; ++i) {
      LOGI("Wi-Fi hidden preflight match[%d] ssid_len=%u channel=%d rssi=%d auth=%d bssid=%s",
           i, WiFi.SSID(i).length(), WiFi.channel(i), WiFi.RSSI(i),
           static_cast<int>(WiFi.encryptionType(i)), WiFi.BSSIDstr(i).c_str());
    }
    WiFi.scanDelete();

    LOGI("Wi-Fi hidden preflight: broad hidden scan start channel=%d", WIFI_CHANNEL);
    int broadCount = WiFi.scanNetworks(false, true, false, 350, WIFI_CHANNEL);
    int hiddenCount = 0;
    int visibleCount = 0;
    int targetVisibleCount = 0;
    int loggedRows = 0;
    for (int i = 0; i < broadCount; ++i) {
      bool hidden = WiFi.SSID(i).isEmpty();
      bool targetVisible = WiFi.SSID(i) == WIFI_SSID;
      if (hidden) hiddenCount++;
      else visibleCount++;
      if (targetVisible) targetVisibleCount++;
      if (hidden && hiddenCandidateCount < MAX_HIDDEN_CANDIDATES) {
        const uint8_t* bssid = WiFi.BSSID(i);
        if (bssid != nullptr) {
          memcpy(hiddenCandidates[hiddenCandidateCount].bssid, bssid, 6);
          hiddenCandidates[hiddenCandidateCount].channel = WiFi.channel(i);
          hiddenCandidates[hiddenCandidateCount].rssi = WiFi.RSSI(i);
          hiddenCandidates[hiddenCandidateCount].auth = static_cast<int>(WiFi.encryptionType(i));
          hiddenCandidateCount++;
        }
      }
      if ((hidden || targetVisible || hasBssid) && loggedRows < 18) {
        bool bssidMatch = hasBssid && strcasecmp(WiFi.BSSIDstr(i).c_str(), configuredBssidString.c_str()) == 0;
        LOGI("Wi-Fi scan row[%d] %s channel=%d rssi=%d auth=%d bssid=%s bssid_match=%d",
             i, hidden ? "hidden" : (targetVisible ? "target-visible" : "other-visible"),
             WiFi.channel(i), WiFi.RSSI(i), static_cast<int>(WiFi.encryptionType(i)),
             WiFi.BSSIDstr(i).c_str(), bssidMatch);
        loggedRows++;
      }
    }
    LOGI("Wi-Fi hidden preflight: broad scan complete total=%d hidden=%d visible=%d target_visible=%d logged=%d",
         broadCount, hiddenCount, visibleCount, targetVisibleCount, loggedRows);
    scanDetail += String("; scan total ") + broadCount + ", hidden " + hiddenCount;
    if (hiddenCandidateCount > 0) scanDetail += String(", candidates ") + hiddenCandidateCount;
    if (targetVisibleCount > 0) scanDetail += String(", target visible ") + targetVisibleCount;
    WiFi.scanDelete();
  } else {
    int targetRssi = -999;
    int targetChannel = 0;
    wifi_auth_mode_t targetAuth = WIFI_AUTH_OPEN;
    bool targetSeen = false;
    int networkCount = WiFi.scanNetworks(false, true);
    for (int i = 0; i < networkCount; ++i) {
      if (WiFi.SSID(i) == WIFI_SSID) {
        targetSeen = true;
        targetRssi = WiFi.RSSI(i);
        targetChannel = WiFi.channel(i);
        targetAuth = WiFi.encryptionType(i);
        break;
      }
    }
    scanDetail = targetSeen
      ? String("SSID seen, RSSI ") + targetRssi + " dBm, ch " + targetChannel + ", auth " + static_cast<int>(targetAuth)
      : String("SSID not seen in 2.4 GHz scan; networks: ") + networkCount;
  }
  setWifiDetail(scanDetail);
  LOGI("Wi-Fi scan/preflight detail: %s", scanDetail.c_str());

  auto tryConnectOnChannel = [&](int channel, uint32_t timeoutMs, const uint8_t* bssidOverride, const char* source, bool demoExact) -> bool {
    const uint8_t* effectiveBssid = bssidOverride;
    bool effectiveHasBssid = effectiveBssid != nullptr;
    if (!effectiveHasBssid && hasBssid) {
      effectiveBssid = configuredBssid;
      effectiveHasBssid = true;
    }
    String effectiveBssidString = effectiveHasBssid ? formatBssid(effectiveBssid) : String("");
    LOGI("Wi-Fi IDF attempt start ssid=%s hidden=%d channel=%d bssid_set=%d bssid=%s source=%s demo_exact=%d timeout_ms=%lu",
         WIFI_SSID, WIFI_HIDDEN, channel, effectiveHasBssid,
         effectiveHasBssid ? effectiveBssidString.c_str() : "none",
         source, demoExact, static_cast<unsigned long>(timeoutMs));
    esp_wifi_disconnect();
    delay(120);

    wifi_config_t cfg = {};
    strlcpy(reinterpret_cast<char*>(cfg.sta.ssid), WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy(reinterpret_cast<char*>(cfg.sta.password), WIFI_PASSWORD, sizeof(cfg.sta.password));
    if (!demoExact) {
      cfg.sta.scan_method = channel > 0 ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
      cfg.sta.channel = channel > 0 ? static_cast<uint8_t>(channel) : 0;
    }
    cfg.sta.threshold.authmode = strlen(WIFI_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    if (effectiveHasBssid) {
      cfg.sta.bssid_set = 1;
      memcpy(cfg.sta.bssid, effectiveBssid, 6);
    }

    esp_err_t configErr = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (configErr != ESP_OK) {
      LOGE("esp_wifi_set_config failed err=0x%x", configErr);
      return false;
    }
    esp_err_t connectErr = esp_wifi_connect();
    if (connectErr != ESP_OK) {
      LOGE("esp_wifi_connect failed err=0x%x", connectErr);
      return false;
    }

    unsigned long start = millis();
    wifi_ap_record_t apInfo = {};
    bool loggedAssociation = false;
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
      if (!loggedAssociation && esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
        loggedAssociation = true;
        LOGI("Wi-Fi AP associated ssid=%s bssid=%02x:%02x:%02x:%02x:%02x:%02x primary=%u rssi=%d auth=%d; waiting for IP",
             reinterpret_cast<const char*>(apInfo.ssid),
             apInfo.bssid[0], apInfo.bssid[1], apInfo.bssid[2],
             apInfo.bssid[3], apInfo.bssid[4], apInfo.bssid[5],
             apInfo.primary, apInfo.rssi, apInfo.authmode);
      }
      ledSet(255, 0, 0, ((millis() / 300) % 2) ? 80 : 8);
      delay(250);
      Serial.print(".");
    }
    Serial.println();

    wl_status_t status = WiFi.status();
    bool idfConnected = esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK;
    LOGI("Wi-Fi attempt complete channel=%d status=%d (%s) elapsed=%lu",
         channel, status, wifiStatusName(status), static_cast<unsigned long>(millis() - start));
    if (idfConnected) {
      LOGI("Wi-Fi AP associated ssid=%s bssid=%02x:%02x:%02x:%02x:%02x:%02x primary=%u rssi=%d auth=%d",
           reinterpret_cast<const char*>(apInfo.ssid),
           apInfo.bssid[0], apInfo.bssid[1], apInfo.bssid[2],
           apInfo.bssid[3], apInfo.bssid[4], apInfo.bssid[5],
           apInfo.primary, apInfo.rssi, apInfo.authmode);
    }
    return status == WL_CONNECTED;
  };

  bool connected = false;
  if (WIFI_HIDDEN) {
    setWifiDetail(scanDetail + "; trying demo-style connect");
    connected = tryConnectOnChannel(0, 20000, nullptr, "demo-style-all-channel", true);

    if (hasBssid) {
      int channel = WIFI_CHANNEL > 0 ? WIFI_CHANNEL : 0;
      setWifiDetail(scanDetail + "; trying configured BSSID");
      connected = connected || tryConnectOnChannel(channel, 20000, configuredBssid, "configured-bssid", false);
    }

    if (!connected && !hasBssid && hiddenCandidateCount > 0) {
      setWifiDetail(scanDetail + "; trying hidden BSSID candidates");
      for (int i = 0; i < hiddenCandidateCount && !connected; ++i) {
        LOGI("Wi-Fi hidden candidate[%d] channel=%d rssi=%d auth=%d bssid=%s",
             i, hiddenCandidates[i].channel, hiddenCandidates[i].rssi, hiddenCandidates[i].auth,
             formatBssid(hiddenCandidates[i].bssid).c_str());
        connected = tryConnectOnChannel(hiddenCandidates[i].channel, 7000,
                                        hiddenCandidates[i].bssid, "scan-hidden-bssid", false);
      }
    }

    if (!connected && WIFI_CHANNEL == 0) {
      setWifiDetail(scanDetail + "; trying channels 1-13");
      for (int channel = 1; channel <= 13 && !connected; ++channel) {
        connected = tryConnectOnChannel(channel, 4500, nullptr, "channel-sweep", false);
      }
    } else if (!connected) {
      connected = tryConnectOnChannel(WIFI_CHANNEL, 20000, nullptr, "configured-channel", false);
    }
  } else {
    connected = tryConnectOnChannel(WIFI_CHANNEL, 20000, nullptr, "visible-ssid", false);
  }

  if (!connected) {
    wl_status_t status = WiFi.status();
    String detail = scanDetail + "; status " + static_cast<int>(status) + " (" + wifiStatusName(status) + ")";
    setWifiDetail(detail);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    ledPulse(255, 0, 0, 3);
    ledSet(255, 0, 0, 50);
    setLastError("Wi-Fi connect failed");
    return false;
  }
  ledPulse(0, 255, 0, 1);
  ledOff();
  setWifiDetail("Connected: " + WiFi.localIP().toString());
  Serial.println(WiFi.localIP());
  return true;
}

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
  body += htmlEscape(String(EDF_EMAIL));
  body += F("'></label>");
  body += F("<label>EDF password<input name='password' type='password' required></label>");
  body += F("<label>EDF account number<input name='account' placeholder='A-AAAA1111' required value='");
  body += htmlEscape(String(EDF_ACCOUNT));
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

bool buttonLow(gpio_num_t pin) {
  return digitalRead(static_cast<uint8_t>(pin)) == LOW;
}

bool refreshButtonWake() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  LOGI("refreshButtonWake wake_cause=%d refresh_pin_low=%d", cause, buttonLow(BTN_REFRESH_PIN));
  if (cause == ESP_SLEEP_WAKEUP_TIMER) return true;
  if (cause != ESP_SLEEP_WAKEUP_EXT1) return false;
  return buttonLow(BTN_REFRESH_PIN);
}

bool applyPageButtonWake() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  LOGI("applyPageButtonWake wake_cause=%d up_low=%d down_low=%d current_page=%s",
       cause, buttonLow(BTN_UP_PIN), buttonLow(BTN_DOWN_PIN), pageNameC(static_cast<Page>(rtcCurrentPage)));
  if (cause != ESP_SLEEP_WAKEUP_EXT1) return false;

  if (buttonLow(BTN_UP_PIN)) {
    rtcCurrentPage = (rtcCurrentPage + PAGE_COUNT - 1) % PAGE_COUNT;
    LOGI("wake page changed via up to %s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    return true;
  } else if (buttonLow(BTN_DOWN_PIN)) {
    rtcCurrentPage = (rtcCurrentPage + 1) % PAGE_COUNT;
    LOGI("wake page changed via down to %s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    return true;
  }
  return false;
}

void prepareButtons() {
  LOGI("configuring buttons up=%d down=%d refresh=%d", BTN_UP_PIN, BTN_DOWN_PIN, BTN_REFRESH_PIN);
  pinMode(static_cast<uint8_t>(BTN_UP_PIN), INPUT_PULLUP);
  pinMode(static_cast<uint8_t>(BTN_DOWN_PIN), INPUT_PULLUP);
  pinMode(static_cast<uint8_t>(BTN_REFRESH_PIN), INPUT_PULLUP);
}

void sleepUntilNextWake() {
  LOGI("Entering deep sleep page=%s has_data=%d last_refresh=%ld",
       pageNameC(static_cast<Page>(rtcCurrentPage)), rtcHasData, static_cast<long>(rtcLastRefresh));
  ledPulse(0, 0, 255, 1);
  ledOff();
  delay(200);

  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  for (gpio_num_t pin : {BTN_UP_PIN, BTN_DOWN_PIN, BTN_REFRESH_PIN}) {
    rtc_gpio_init(pin);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(pin);
    rtc_gpio_pullup_en(pin);
  }

  esp_sleep_enable_ext1_wakeup(
    (1ULL << BTN_UP_PIN) | (1ULL << BTN_DOWN_PIN) | (1ULL << BTN_REFRESH_PIN),
    ESP_EXT1_WAKEUP_ANY_LOW
  );
  esp_sleep_enable_timer_wakeup(ONE_HOUR_US);
  esp_deep_sleep_start();
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.clear_display = false;
  cfg.led_brightness = 64;
  M5.begin(cfg);
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== EDF Paper Color ===");
  LOGI("boot start reset_reason=%d wake_cause=%d board=%d psram=%d heap=%u",
       esp_reset_reason(), esp_sleep_get_wakeup_cause(), static_cast<int>(M5.getBoard()),
       psramFound(), ESP.getFreeHeap());

  initPaperColorHardware();
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  ledPulse(255, 0, 102, 1);
  prepareButtons();
  M5.Display.setRotation(0);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  LOGI("display ready width=%d height=%d led_enabled=%d led_count=%u",
       M5.Display.width(), M5.Display.height(), M5.Led.isEnabled(), M5.Led.getCount());
  awakeSinceMs = millis();

  loadConfig();
  bool pageChangedOnWake = applyPageButtonWake();

  bool wifiOk = connectWifi();
  LOGI("wifi result ok=%d status=%d ip=%s detail=%s",
       wifiOk, WiFi.status(), wifiOk ? WiFi.localIP().toString().c_str() : "none", rtcWifiDetail);
  if (wifiOk) {
    syncTime();
    startWebServer();
  }

  setupMode = !config.configured || config.refreshToken.length() == 0;
  if (setupMode && wifiOk && hasSetupHeaderCredentials()) {
    LOGI("setup.h EDF account fields present; attempting automatic provisioning");
    bool refreshed = provisionEdfAccount(String(EDF_EMAIL), String(EDF_PASSWORD), String(EDF_ACCOUNT), false);
    LOGI("setup.h automatic provisioning complete configured=%d refreshed=%d error=%s",
         config.configured, refreshed, rtcLastError);
    setupMode = !config.configured || config.refreshToken.length() == 0;
  }
  LOGI("mode decision setupMode=%d configured=%d refresh_token_present=%d",
       setupMode, config.configured, config.refreshToken.length() > 0);
  if (setupMode) {
    drawStatusScreen("Setup required", wifiOk ? "Open the setup page." : "Wi-Fi connection failed.");
    return;
  }

  tokenState.refreshToken = config.refreshToken;
  bool shouldRefresh = refreshButtonWake() || pageChangedOnWake || !rtcHasData || rtcLastRefresh == 0 || nowUtc() - rtcLastRefresh >= 3600;
  LOGI("refresh decision shouldRefresh=%d has_data=%d last_refresh=%ld age=%ld wifi_ok=%d",
       shouldRefresh, rtcHasData, static_cast<long>(rtcLastRefresh),
       static_cast<long>(nowUtc() - rtcLastRefresh), wifiOk);
  if (shouldRefresh && wifiOk) {
    bool ok = refreshData();
    LOGI("startup refresh complete ok=%d", ok);
  }

  if (rtcHasData) {
    drawDashboard();
  } else {
    drawStatusScreen("No data", "Refresh failed. G1 retries.");
  }

  // Keep the status web server briefly reachable after a manual wake, then sleep.
  awakeSinceMs = millis();
  LOGI("setup complete awake timer reset");
}

void loop() {
  if (webStarted) server.handleClient();

  if (setupMode || !config.configured) {
    delay(20);
    return;
  }

  static bool upLast = HIGH;
  static bool downLast = HIGH;
  static bool refreshLast = HIGH;
  bool up = digitalRead(static_cast<uint8_t>(BTN_UP_PIN));
  bool down = digitalRead(static_cast<uint8_t>(BTN_DOWN_PIN));
  bool refresh = digitalRead(static_cast<uint8_t>(BTN_REFRESH_PIN));

  if (up == LOW && upLast == HIGH) {
    LOGI("button up pressed page_before=%s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    rtcCurrentPage = (rtcCurrentPage + PAGE_COUNT - 1) % PAGE_COUNT;
    refreshData();
    drawDashboard();
    LOGI("button up handled page_after=%s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    awakeSinceMs = millis();
  } else if (down == LOW && downLast == HIGH) {
    LOGI("button down pressed page_before=%s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    rtcCurrentPage = (rtcCurrentPage + 1) % PAGE_COUNT;
    refreshData();
    drawDashboard();
    LOGI("button down handled page_after=%s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    awakeSinceMs = millis();
  } else if (refresh == LOW && refreshLast == HIGH) {
    LOGI("button refresh pressed");
    bool ok = refreshData();
    if (rtcHasData) {
      drawDashboard();
    } else {
      drawStatusScreen("Refresh failed", "No energy data available.");
    }
    LOGI("button refresh handled ok=%d", ok);
    awakeSinceMs = millis();
  }

  upLast = up;
  downLast = down;
  refreshLast = refresh;

  if (millis() - awakeSinceMs > 15000) {
    LOGI("awake timeout elapsed=%lu", millis() - awakeSinceMs);
    sleepUntilNextWake();
  }

  delay(50);
}
