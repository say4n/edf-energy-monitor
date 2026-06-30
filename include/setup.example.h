#pragma once

// Copy this file to include/setup.h and fill in your Wi-Fi details.
// include/setup.h is ignored by git.
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// Set to 1 for a hidden SSID. For hidden SSIDs, setting the exact 2.4 GHz
// channel is strongly recommended; leave as 0 only if you want the firmware to
// try channels 1-13 in sequence.
#define WIFI_HIDDEN 0
#define WIFI_CHANNEL 0

// Optional, but useful for hidden SSIDs. Use the 2.4 GHz AP radio MAC address,
// for example "aa:bb:cc:dd:ee:ff". Leave blank to connect by SSID only.
#define WIFI_BSSID ""

// Optional EDF setup details. If all three are set, the device provisions
// itself on first boot after Wi-Fi connects. Leave blank to use the web setup UI.
#define EDF_EMAIL ""
#define EDF_PASSWORD ""
#define EDF_ACCOUNT ""
