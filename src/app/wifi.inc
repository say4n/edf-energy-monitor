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
