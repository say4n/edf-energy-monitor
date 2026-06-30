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
