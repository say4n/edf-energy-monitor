#pragma once

#include "app/state.h"

namespace app {

// LED and hardware
void ledOff();
void ledSet(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness = 60);
void ledPulse(uint8_t red, uint8_t green, uint8_t blue, int count = 1);
void initPaperColorHardware();

// Power / status helpers
String batteryLabel();

// Formatting helpers
String formatBssid(const uint8_t* bssid);
String formatAsciiHex(const char* value);
bool parseBssid(const char* value, uint8_t out[6]);
const char* wifiStatusName(wl_status_t status);

// QR rendering (used by UI)
bool drawQrCode(const String& url, int x, int y, int maxSize);

// String / HTML helpers (used by UI)
String htmlEscape(const String& input);
String ellipsizeText(const String& text, int maxWidth);
void drawClippedString(const String& text, int x, int y, int maxWidth);

// Setup config helpers
bool hasSetupHeaderCredentials();

// Error tracking
void setLastError(const String& message);
void clearLastError();

// GraphQL helpers (used by EDF module)
String graphqlErrorSummary(JsonDocument& response);
void logGraphqlResponseShape(JsonDocument& response, const char* context);

// Time helpers
time_t nowUtc();
String isoUtc(time_t t);
String dateLabel(time_t t, const char* fmt);
time_t localTimeAt(int year, int mon, int mday, int hour = 0, int min = 0, int sec = 0);
time_t startOfTodayLocal();
time_t startOfWeekLocal();
time_t startOf30DayWindowLocal();
time_t startOfYearLocal();
int daysIn30DayWindow();
bool parseIsoTime(const char* value, time_t* out);

// Dashboard label helpers (used by render)
String dashboardStatusLabel();
String dashboardRefreshLabel(time_t refreshedAt);

// Unit conversion
float convertGasM3ToKwh(float m3);

// Bucket comparison helpers (used by EDF module)
bool nearlyEqualFloat(float left, float right);
bool bucketsEqual(const Bucket* left, const Bucket* right, int count);
void copyBuckets(Bucket* dest, const Bucket* src, int count);
int bucketIndexFor(time_t t, Page page, time_t pageStart);

// NVS config persistence
void saveConfig();
void loadConfig();
void clearConfig();

// Buttons and sleep
void prepareButtons();
bool refreshButtonWake();
bool applyPageButtonWake();
void sleepUntilNextWake();

// Wi-Fi (forward declared in state.h, defined here)
bool connectWifi();
bool ensureWifiConnected(const char* context);
void syncTime();

}  // namespace app
