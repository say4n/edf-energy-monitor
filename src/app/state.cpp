#include "app/state.h"

namespace app {

RTC_DATA_ATTR Bucket rtcWeek[7];
RTC_DATA_ATTR Bucket rtcMonth[MAX_MONTH_DAYS];
RTC_DATA_ATTR Bucket rtcYear[YEAR_MONTHS];
RTC_DATA_ATTR uint32_t rtcStateVersion = 0;
RTC_DATA_ATTR int rtcMonthDays = 0;
RTC_DATA_ATTR int rtcCurrentPage = PAGE_WEEK;
RTC_DATA_ATTR bool rtcHasData = false;
RTC_DATA_ATTR bool rtcPageHasData[PAGE_COUNT] = {false, false, false};
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
bool wifiConnectInProgress = false;
int qrDrawX = 0;
int qrDrawY = 0;
int qrDrawMaxSize = 0;

}  // namespace app
