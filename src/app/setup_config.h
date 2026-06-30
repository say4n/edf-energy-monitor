#pragma once

#if __has_include("setup.h")
#include "setup.h"
#define APP_HAS_LOCAL_SETUP_HEADER 1
#else
#warning "include/setup.h not found; using placeholder Wi-Fi credentials"
#define WIFI_SSID "replace-me"
#define WIFI_PASSWORD "replace-me"
#define APP_HAS_LOCAL_SETUP_HEADER 0
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

namespace app_config {

constexpr bool hasLocalSetupHeader = APP_HAS_LOCAL_SETUP_HEADER;

constexpr const char* wifiSsid = WIFI_SSID;
constexpr const char* wifiPassword = WIFI_PASSWORD;
constexpr bool wifiHidden = WIFI_HIDDEN != 0;
constexpr int wifiChannel = WIFI_CHANNEL;
constexpr const char* wifiBssid = WIFI_BSSID;

constexpr const char* edfEmail = EDF_EMAIL;
constexpr const char* edfPassword = EDF_PASSWORD;
constexpr const char* edfAccount = EDF_ACCOUNT;

}  // namespace app_config
