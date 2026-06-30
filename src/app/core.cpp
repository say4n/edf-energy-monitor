#include "app/core.h"

#include <esp_sntp.h>

namespace app {

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

void ledSet(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness) {
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

void ledPulse(uint8_t red, uint8_t green, uint8_t blue, int count) {
  LOGI("LED pulse rgb=(%u,%u,%u) count=%d", red, green, blue, count);
  for (int i = 0; i < count; ++i) {
    ledSet(red, green, blue, 90);
    delay(120);
    ledOff();
    delay(120);
  }
}

String batteryLabel() {
  int32_t level = M5.Power.getBatteryLevel();
  if (level >= 0 && level <= 100) return "Bat " + String(level) + "%";

  int16_t voltage = M5.Power.getBatteryVoltage();
  if (voltage > 0) return "Bat " + String(voltage) + "mV";

  return "Bat --";
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

namespace {

bool parseHexByte(const char* value, uint8_t* out) {
  if (!isxdigit(value[0]) || !isxdigit(value[1])) return false;
  char buf[3] = {value[0], value[1], '\0'};
  char* end = nullptr;
  long parsed = strtol(buf, &end, 16);
  if (end == buf || parsed < 0 || parsed > 255) return false;
  *out = static_cast<uint8_t>(parsed);
  return true;
}

}  // namespace

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

namespace {

// Global state used by the QR rendering callback. Kept file-local because the
// callback signature used by esp_qrcode does not accept a context pointer.
int qrDrawX = 0;
int qrDrawY = 0;
int qrDrawMaxSize = 0;

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

}  // namespace

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
  return strlen(app_config::edfEmail) > 0 && strlen(app_config::edfPassword) > 0 && strlen(app_config::edfAccount) > 0;
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

time_t localTimeAt(int year, int mon, int mday, int hour, int min, int sec) {
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

time_t startOf30DayWindowLocal() {
  return startOfTodayLocal() - 29 * 86400;
}

time_t startOfYearLocal() {
  time_t n = nowUtc();
  struct tm tm {};
  localtime_r(&n, &tm);
  return localTimeAt(tm.tm_year + 1900, tm.tm_mon + 1 - 11, 1);
}

int daysIn30DayWindow() {
  return 30;
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

String dashboardStatusLabel() {
  return WiFi.isConnected() ? WiFi.localIP().toString() : "offline";
}

String dashboardRefreshLabel(time_t refreshedAt) {
  return refreshedAt > 0 ? dateLabel(refreshedAt, "%d %b %H:%M") : "not refreshed";
}

float convertGasM3ToKwh(float m3) {
  // Same common conversion shape used by energy suppliers:
  // m3 * correction factor * calorific value / kWh conversion factor.
  constexpr float correction = 1.02264f;
  constexpr float calorific = 39.5f;
  constexpr float kwhFactor = 3.6f;
  return m3 * correction * calorific / kwhFactor;
}

bool nearlyEqualFloat(float left, float right) {
  float diff = fabsf(left - right);
  float scale = max(max(fabsf(left), fabsf(right)), 1.0f);
  return diff <= 0.005f || diff <= scale * 0.0001f;
}

bool bucketsEqual(const Bucket* left, const Bucket* right, int count) {
  if (count <= 0) return true;
  for (int i = 0; i < count; ++i) {
    if (!nearlyEqualFloat(left[i].electricityKwh, right[i].electricityKwh) ||
        !nearlyEqualFloat(left[i].gasKwh, right[i].gasKwh) ||
        !nearlyEqualFloat(left[i].electricityCost, right[i].electricityCost) ||
        !nearlyEqualFloat(left[i].gasCost, right[i].gasCost)) {
      return false;
    }
  }
  return true;
}

void copyBuckets(Bucket* dest, const Bucket* src, int count) {
  if (count > 0) memcpy(dest, src, sizeof(Bucket) * count);
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

// ---------------------------------------------------------------------------
// NVS config persistence
// ---------------------------------------------------------------------------

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
  prefs.putString("gasUnits", config.gas.consumptionUnits);
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
  config.gas.consumptionUnits = prefs.getString("gasUnits", "m3");
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
  for (int i = 0; i < PAGE_COUNT; ++i) rtcPageHasData[i] = false;
}

// ---------------------------------------------------------------------------
// Buttons and sleep
// ---------------------------------------------------------------------------

namespace {

bool buttonLow(gpio_num_t pin) {
  return digitalRead(static_cast<uint8_t>(pin)) == LOW;
}

uint64_t ext1WakeMask() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
}

bool wakePinTriggered(gpio_num_t pin) {
  return (ext1WakeMask() & (1ULL << pin)) != 0;
}

}  // namespace

void prepareButtons() {
  LOGI("configuring buttons up=%d down=%d refresh=%d", BTN_UP_PIN, BTN_DOWN_PIN, BTN_REFRESH_PIN);
  pinMode(static_cast<uint8_t>(BTN_UP_PIN), INPUT_PULLUP);
  pinMode(static_cast<uint8_t>(BTN_DOWN_PIN), INPUT_PULLUP);
  pinMode(static_cast<uint8_t>(BTN_REFRESH_PIN), INPUT_PULLUP);
}

bool refreshButtonWake() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  uint64_t mask = ext1WakeMask();
  LOGI("refreshButtonWake wake_cause=%d wake_mask=0x%llx refresh_triggered=%d refresh_pin_low=%d",
       cause, static_cast<unsigned long long>(mask), wakePinTriggered(BTN_REFRESH_PIN), buttonLow(BTN_REFRESH_PIN));
  if (cause == ESP_SLEEP_WAKEUP_TIMER) return true;
  if (cause != ESP_SLEEP_WAKEUP_EXT1) return false;
  return wakePinTriggered(BTN_REFRESH_PIN);
}

bool applyPageButtonWake() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  uint64_t mask = ext1WakeMask();
  bool upWake = wakePinTriggered(BTN_UP_PIN);
  bool downWake = wakePinTriggered(BTN_DOWN_PIN);
  LOGI("applyPageButtonWake wake_cause=%d wake_mask=0x%llx up_wake=%d down_wake=%d up_low=%d down_low=%d current_page=%s",
       cause, static_cast<unsigned long long>(mask), upWake, downWake,
       buttonLow(BTN_UP_PIN), buttonLow(BTN_DOWN_PIN), pageNameC(static_cast<Page>(rtcCurrentPage)));
  if (cause != ESP_SLEEP_WAKEUP_EXT1) return false;

  if (upWake) {
    rtcCurrentPage = (rtcCurrentPage + PAGE_COUNT - 1) % PAGE_COUNT;
    LOGI("wake page changed via up to %s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    return true;
  } else if (downWake) {
    rtcCurrentPage = (rtcCurrentPage + 1) % PAGE_COUNT;
    LOGI("wake page changed via down to %s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    return true;
  }
  return false;
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

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

bool connectWifi() {
  wifiConnectInProgress = true;
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

  setWifiDetail(app_config::hasLocalSetupHeader
    ? "Wi-Fi config: include/setup.h loaded"
    : "Wi-Fi config: include/setup.h missing");

  ledSet(255, 255, 255, 70);
  uint8_t configuredBssid[6] = {};
  bool hasBssid = parseBssid(app_config::wifiBssid, configuredBssid);
  String configuredBssidString = hasBssid ? formatBssid(configuredBssid) : String("");
  LOGI("Wi-Fi target ssid=%s ssid_len=%u ssid_hex=%s password_len=%u hidden=%d configured_channel=%d bssid_set=%d",
       app_config::wifiSsid, strlen(app_config::wifiSsid), formatAsciiHex(app_config::wifiSsid).c_str(),
       strlen(app_config::wifiPassword), app_config::wifiHidden, app_config::wifiChannel, hasBssid);
  if (app_config::wifiBssid[0] != '\0' && !hasBssid) {
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
  if (app_config::wifiHidden) {
    scanDetail = String("Hidden SSID mode");
    if (app_config::wifiChannel > 0) scanDetail += String("; channel ") + app_config::wifiChannel;
    if (hasBssid) scanDetail += "; BSSID set";

    LOGI("Wi-Fi hidden preflight: active SSID scan start channel=%d bssid_set=%d",
         app_config::wifiChannel, hasBssid);
    int specificCount = WiFi.scanNetworks(false, true, false, 600, app_config::wifiChannel, app_config::wifiSsid,
                                          hasBssid ? configuredBssid : nullptr);
    LOGI("Wi-Fi hidden preflight: active SSID scan complete count=%d", specificCount);
    for (int i = 0; i < specificCount; ++i) {
      LOGI("Wi-Fi hidden preflight match[%d] ssid_len=%u channel=%d rssi=%d auth=%d bssid=%s",
           i, WiFi.SSID(i).length(), WiFi.channel(i), WiFi.RSSI(i),
           static_cast<int>(WiFi.encryptionType(i)), WiFi.BSSIDstr(i).c_str());
    }
    WiFi.scanDelete();

    LOGI("Wi-Fi hidden preflight: broad hidden scan start channel=%d", app_config::wifiChannel);
    int broadCount = WiFi.scanNetworks(false, true, false, 350, app_config::wifiChannel);
    int hiddenCount = 0;
    int visibleCount = 0;
    int targetVisibleCount = 0;
    int loggedRows = 0;
    for (int i = 0; i < broadCount; ++i) {
      bool hidden = WiFi.SSID(i).isEmpty();
      bool targetVisible = WiFi.SSID(i) == app_config::wifiSsid;
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
      if (WiFi.SSID(i) == app_config::wifiSsid) {
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
    WiFi.scanDelete();
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
         app_config::wifiSsid, app_config::wifiHidden, channel, effectiveHasBssid,
         effectiveHasBssid ? effectiveBssidString.c_str() : "none",
         source, demoExact, static_cast<unsigned long>(timeoutMs));
    esp_wifi_disconnect();
    delay(120);

    wifi_config_t cfg = {};
    strlcpy(reinterpret_cast<char*>(cfg.sta.ssid), app_config::wifiSsid, sizeof(cfg.sta.ssid));
    strlcpy(reinterpret_cast<char*>(cfg.sta.password), app_config::wifiPassword, sizeof(cfg.sta.password));
    if (!demoExact) {
      cfg.sta.scan_method = channel > 0 ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
      cfg.sta.channel = channel > 0 ? static_cast<uint8_t>(channel) : 0;
    }
    cfg.sta.threshold.authmode = strlen(app_config::wifiPassword) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
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
      ledSet(255, 255, 255, ((millis() / 300) % 2) ? 80 : 8);
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
  if (app_config::wifiHidden) {
    setWifiDetail(scanDetail + "; trying demo-style connect");
    connected = tryConnectOnChannel(0, 20000, nullptr, "demo-style-all-channel", true);

    if (hasBssid) {
      int channel = app_config::wifiChannel > 0 ? app_config::wifiChannel : 0;
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

    if (!connected && app_config::wifiChannel == 0) {
      setWifiDetail(scanDetail + "; trying channels 1-13");
      for (int channel = 1; channel <= 13 && !connected; ++channel) {
        connected = tryConnectOnChannel(channel, 4500, nullptr, "channel-sweep", false);
      }
    } else if (!connected) {
      connected = tryConnectOnChannel(app_config::wifiChannel, 20000, nullptr, "configured-channel", false);
    }
  } else {
    connected = tryConnectOnChannel(app_config::wifiChannel, 20000, nullptr, "visible-ssid", false);
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
    wifiConnectInProgress = false;
    return false;
  }
  unsigned long ipStart = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - ipStart < 5000) {
    delay(100);
  }
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  ledPulse(0, 255, 0, 1);
  ledOff();
  setWifiDetail("Connected: " + WiFi.localIP().toString());
  Serial.println(WiFi.localIP());
  wifiConnectInProgress = false;
  return true;
}

bool ensureWifiConnected(const char* context) {
  if (WiFi.isConnected() && WiFi.localIP() != IPAddress(0, 0, 0, 0)) return true;
  if (wifiConnectInProgress) return false;

  LOGW("Wi-Fi not connected before %s status=%d (%s); reconnecting",
       context, WiFi.status(), wifiStatusName(WiFi.status()));
  WiFi.reconnect();
  unsigned long start = millis();
  while ((!WiFi.isConnected() || WiFi.localIP() == IPAddress(0, 0, 0, 0)) && millis() - start < 6000) {
    delay(200);
  }
  if (WiFi.isConnected() && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    setWifiDetail("Reconnected: " + WiFi.localIP().toString());
    return true;
  }

  LOGW("fast reconnect failed before %s; running full connect", context);
  return connectWifi();
}

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

}  // namespace app
