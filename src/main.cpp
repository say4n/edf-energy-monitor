#include <Arduino.h>

#include "app/state.h"
#include "app/core.h"
#include "app/edf.h"
#include "app/ui.h"

using namespace app;

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
  if (rtcStateVersion != RTC_STATE_VERSION) {
    LOGI("RTC state version changed old=%u new=%u; clearing cached page data",
         rtcStateVersion, RTC_STATE_VERSION);
    rtcStateVersion = RTC_STATE_VERSION;
    rtcHasData = false;
    for (int i = 0; i < PAGE_COUNT; ++i) rtcPageHasData[i] = false;
  }
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
    bool refreshed = provisionEdfAccount(String(app_config::edfEmail), String(app_config::edfPassword), String(app_config::edfAccount), false);
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
  bool refreshWake = refreshButtonWake();
  bool pageHasData = currentPageHasData();
  bool shouldRefresh = wifiOk;
  LOGI("refresh decision shouldRefresh=%d page_changed=%d refresh_wake=%d page_has_data=%d any_data=%d last_refresh=%ld wifi_ok=%d",
       shouldRefresh, pageChangedOnWake, refreshWake, pageHasData, rtcHasData,
       static_cast<long>(rtcLastRefresh), wifiOk);
  if (shouldRefresh && wifiOk) {
    bool displayChanged = false;
    bool ok = refreshData(&displayChanged);
    LOGI("startup refresh complete ok=%d display_changed=%d", ok, displayChanged);
  }

  if (currentPageHasData()) {
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
    int previousPage = rtcCurrentPage;
    LOGI("button up pressed page_before=%s", pageNameC(static_cast<Page>(previousPage)));
    rtcCurrentPage = (rtcCurrentPage + PAGE_COUNT - 1) % PAGE_COUNT;
    bool displayChanged = false;
    bool ok = refreshData(&displayChanged);
    if (ok) {
      drawDashboard();
    } else {
      rtcCurrentPage = previousPage;
      LOGW("button up keeping previous screen after failed refresh page=%s", pageNameC(static_cast<Page>(previousPage)));
    }
    LOGI("button up handled ok=%d page_after=%s", ok, pageNameC(static_cast<Page>(rtcCurrentPage)));
    awakeSinceMs = millis();
  } else if (down == LOW && downLast == HIGH) {
    int previousPage = rtcCurrentPage;
    LOGI("button down pressed page_before=%s", pageNameC(static_cast<Page>(previousPage)));
    rtcCurrentPage = (rtcCurrentPage + 1) % PAGE_COUNT;
    bool displayChanged = false;
    bool ok = refreshData(&displayChanged);
    if (ok) {
      drawDashboard();
    } else {
      rtcCurrentPage = previousPage;
      LOGW("button down keeping previous screen after failed refresh page=%s", pageNameC(static_cast<Page>(previousPage)));
    }
    LOGI("button down handled ok=%d page_after=%s", ok, pageNameC(static_cast<Page>(rtcCurrentPage)));
    awakeSinceMs = millis();
  } else if (refresh == LOW && refreshLast == HIGH) {
    LOGI("button refresh pressed");
    bool displayChanged = false;
    bool ok = refreshData(&displayChanged);
    if (currentPageHasData()) {
      if (displayChanged) drawDashboard();
    } else {
      drawStatusScreen("Refresh failed", "No energy data available.");
    }
    LOGI("button refresh handled ok=%d display_changed=%d", ok, displayChanged);
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
