# edf-energy-monitor

M5Stack Paper Color based energy monitor for EDF.

![EDF Energy Monitor on M5Stack Paper Color](/assets/demo.jpeg)

## Setup

1. Install PlatformIO.
2. Copy `include/setup.example.h` to `include/setup.h`.
3. Set `WIFI_SSID` and `WIFI_PASSWORD` in `include/setup.h`.
   For a hidden network, also set `WIFI_HIDDEN 1`. If known, set the 2.4 GHz
   `WIFI_CHANNEL` and optional 2.4 GHz AP `WIFI_BSSID` to make association
   more reliable.
   Optionally set `EDF_EMAIL`, `EDF_PASSWORD`, and `EDF_ACCOUNT` in the same
   file to provision the EDF account without using the web setup form.
4. Build and upload:

```sh
pio run -e m5stack-papercolor
pio run -e m5stack-papercolor -t upload
```

On first boot, the device connects to Wi-Fi and shows a local setup URL. Open it on the same network, enter your EDF email, password, and account number, then the device stores only the EDF refresh token.
