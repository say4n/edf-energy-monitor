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
