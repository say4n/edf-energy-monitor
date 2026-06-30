bool obtainRefreshToken(const String& email, const String& password, String* refreshToken) {
  LOGI("obtaining initial refresh token for email_present=%d password_present=%d",
       email.length() > 0, password.length() > 0);
  JsonDocument payload;
  payload["query"] =
    "mutation ObtainToken($email: String!, $password: String!) {"
    "  obtainKrakenToken(input: { email: $email, password: $password }) {"
    "    token refreshToken refreshExpiresIn"
    "  }"
    "}";
  payload["variables"]["email"] = email;
  payload["variables"]["password"] = password;

  String body;
  serializeJson(payload, body);

  JsonDocument response;
  if (!postJson(GRAPHQL_URL, body, response, "", "initial-token")) return false;
  logGraphqlResponseShape(response, "initial-token");

  String token = response["data"]["obtainKrakenToken"]["refreshToken"].as<String>();
  LOGI("initial token fields access_len=%u refresh_len=%u",
       response["data"]["obtainKrakenToken"]["token"].as<String>().length(), token.length());
  if (token.length() == 0) {
    String errors = graphqlErrorSummary(response);
    setLastError(errors.length() > 0 ? ("EDF login failed: " + errors) : "EDF login failed: no refresh token");
    return false;
  }

  *refreshToken = token;
  LOGI("initial refresh token obtained");
  return true;
}

bool refreshAccessToken() {
  LOGI("refreshAccessToken start access_present=%d refresh_present=%d access_expires_in=%ld",
       tokenState.accessToken.length() > 0,
       (tokenState.refreshToken.length() > 0 || config.refreshToken.length() > 0),
       static_cast<long>(tokenState.accessExpiresAt - nowUtc()));
  if (tokenState.accessToken.length() > 0 && tokenState.accessExpiresAt - 300 > nowUtc()) {
    LOGI("using cached access token");
    return true;
  }

  String refreshToken = tokenState.refreshToken.length() > 0 ? tokenState.refreshToken : config.refreshToken;
  if (refreshToken.length() == 0) {
    setLastError("Missing refresh token");
    return false;
  }

  JsonDocument payload;
  payload["query"] =
    "mutation RefreshToken($refreshToken: String!) {"
    "  obtainKrakenToken(input: { refreshToken: $refreshToken }) {"
    "    token refreshToken refreshExpiresIn"
    "  }"
    "}";
  payload["variables"]["refreshToken"] = refreshToken;

  String body;
  serializeJson(payload, body);

  JsonDocument response;
  if (!postJson(GRAPHQL_URL, body, response, "", "refresh-token")) return false;
  logGraphqlResponseShape(response, "refresh-token");

  JsonObject token = response["data"]["obtainKrakenToken"];
  String access = token["token"].as<String>();
  String rotated = token["refreshToken"].as<String>();
  LOGI("refresh token fields access_len=%u refresh_len=%u",
       access.length(), rotated.length());
  if (access.length() == 0 || rotated.length() == 0) {
    String errors = graphqlErrorSummary(response);
    setLastError(errors.length() > 0 ? ("Token refresh failed: " + errors) : "Token refresh failed: missing token fields");
    return false;
  }

  tokenState.accessToken = access;
  tokenState.refreshToken = rotated;
  tokenState.accessExpiresAt = nowUtc() + 3600;
  tokenState.refreshExpiresAt = token["refreshExpiresIn"] | 0;
  LOGI("access token refreshed refresh_expires_at=%ld rotated=%d",
       static_cast<long>(tokenState.refreshExpiresAt), config.refreshToken != tokenState.refreshToken);

  if (config.refreshToken != tokenState.refreshToken) {
    LOGI("refresh token rotated; saving updated token");
    config.refreshToken = tokenState.refreshToken;
    saveConfig();
  }
  return true;
}

String authHeader(bool jwtPrefix = true) {
  return jwtPrefix ? "JWT " + tokenState.accessToken : tokenState.accessToken;
}

bool discoverAccount(const String& accountNumber) {
  LOGI("discovering account account=%s", accountNumber.c_str());
  if (!refreshAccessToken()) return false;

  JsonDocument payload;
  String query =
    "query {"
    "  account(accountNumber: \"" + accountNumber + "\") {"
    "    electricityAgreements(active: true) {"
    "      meterPoint {"
    "        mpan"
    "        meters(includeInactive: false) { serialNumber smartImportElectricityMeter { deviceId } }"
    "        agreements(includeInactive: true) { validFrom validTo tariff { ... on TariffType { productCode tariffCode } } }"
    "      }"
    "    }"
    "    gasAgreements(active: true) {"
    "      meterPoint {"
    "        mprn"
    "        meters(includeInactive: false) { serialNumber consumptionUnits smartGasMeter { deviceId } }"
    "        agreements(includeInactive: true) { validFrom validTo tariff { productCode tariffCode } }"
    "      }"
    "    }"
    "  }"
    "}";
  payload["query"] = query;

  String body;
  serializeJson(payload, body);

  JsonDocument response;
  if (!postJson(GRAPHQL_URL, body, response, authHeader(false), "get-account")) return false;

  JsonObject account = response["data"]["account"];
  if (account.isNull()) {
    setLastError("Account not found");
    return false;
  }

  config.electricity = FuelConfig {};
  config.electricity.isElectricity = true;
  for (JsonObject agreement : account["electricityAgreements"].as<JsonArray>()) {
    JsonObject meterPoint = agreement["meterPoint"];
    JsonObject meter = meterPoint["meters"][0];
    JsonObject tariffAgreement = meterPoint["agreements"][0];
    const char* product = tariffAgreement["tariff"]["productCode"] | "";
    const char* tariff = tariffAgreement["tariff"]["tariffCode"] | "";
    if (meterPoint["mpan"].is<const char*>() && meter["serialNumber"].is<const char*>() && strlen(product) > 0 && strlen(tariff) > 0) {
      config.electricity.present = true;
      config.electricity.pointId = meterPoint["mpan"].as<const char*>();
      config.electricity.serial = meter["serialNumber"].as<const char*>();
      config.electricity.smartMeter = !meter["smartImportElectricityMeter"].isNull();
      config.electricity.productCode = product;
      config.electricity.tariffCode = tariff;
      LOGI("discovered electricity mpan=%s serial=%s product=%s tariff=%s smart=%d",
           config.electricity.pointId.c_str(), config.electricity.serial.c_str(),
           config.electricity.productCode.c_str(), config.electricity.tariffCode.c_str(),
           config.electricity.smartMeter);
      break;
    }
  }

  config.gas = FuelConfig {};
  config.gas.isElectricity = false;
  for (JsonObject agreement : account["gasAgreements"].as<JsonArray>()) {
    JsonObject meterPoint = agreement["meterPoint"];
    JsonObject meter = meterPoint["meters"][0];
    JsonObject tariffAgreement = meterPoint["agreements"][0];
    const char* product = tariffAgreement["tariff"]["productCode"] | "";
    const char* tariff = tariffAgreement["tariff"]["tariffCode"] | "";
    if (meterPoint["mprn"].is<const char*>() && meter["serialNumber"].is<const char*>() && strlen(product) > 0 && strlen(tariff) > 0) {
      config.gas.present = true;
      config.gas.pointId = meterPoint["mprn"].as<const char*>();
      config.gas.serial = meter["serialNumber"].as<const char*>();
      config.gas.productCode = product;
      config.gas.tariffCode = tariff;
      config.gas.consumptionUnits = meter["consumptionUnits"] | "m3";
      LOGI("discovered gas mprn=%s serial=%s product=%s tariff=%s units=%s",
           config.gas.pointId.c_str(), config.gas.serial.c_str(),
           config.gas.productCode.c_str(), config.gas.tariffCode.c_str(),
           config.gas.consumptionUnits.c_str());
      break;
    }
  }

  if (!config.electricity.present && !config.gas.present) {
    setLastError("No meters discovered");
    return false;
  }

  config.accountNumber = accountNumber;
  config.configured = true;
  saveConfig();
  LOGI("account discovery complete elec=%d gas=%d", config.electricity.present, config.gas.present);
  return true;
}

float rateAt(JsonArray rates, time_t slotStart) {
  for (JsonObject rate : rates) {
    time_t start = 0, end = 0;
    const char* startText = rate["valid_from"];
    if (!startText) startText = rate["start"];
    const char* endText = rate["valid_to"];
    if (!endText) endText = rate["end"];
    if (!parseIsoTime(startText, &start)) continue;
    bool hasEnd = parseIsoTime(endText, &end);
    if (slotStart >= start && (!hasEnd || slotStart < end)) {
      return rate["value_inc_vat"] | 0.0f;
    }
  }
  return 0;
}

bool fetchRates(const FuelConfig& fuel, time_t from, time_t to, JsonDocument& ratesOut) {
  LOGI("fetchRates fuel=%s present=%d point=%s product=%s tariff=%s from=%s to=%s",
       fuel.isElectricity ? "electricity" : "gas", fuel.present, fuel.pointId.c_str(),
       fuel.productCode.c_str(), fuel.tariffCode.c_str(), isoUtc(from).c_str(), isoUtc(to).c_str());
  if (fuel.productCode.length() == 0 || fuel.tariffCode.length() == 0) return false;

  String url;
  if (fuel.isElectricity) {
    url = String(EDF_BASE_URL) + "/v1/products/" + fuel.productCode +
          "/electricity-tariffs/" + fuel.tariffCode +
          "/standard-unit-rates/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=1500";
  } else {
    url = String(EDF_BASE_URL) + "/v1/products/" + fuel.productCode +
          "/gas-tariffs/" + fuel.tariffCode +
          "/standard-unit-rates/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=1500";
  }

  if (!getJson(url, ratesOut, authHeader(), fuel.isElectricity ? "electricity-rates" : "gas-rates")) {
    return false;
  }
  LOGI("fetchRates complete fuel=%s result_count=%u",
       fuel.isElectricity ? "electricity" : "gas", ratesOut["results"].as<JsonArray>().size());
  return true;
}

bool fetchStandingCharge(const FuelConfig& fuel, time_t from, time_t to, float* standingChargePence) {
  LOGI("fetchStandingCharge fuel=%s from=%s to=%s",
       fuel.isElectricity ? "electricity" : "gas", isoUtc(from).c_str(), isoUtc(to).c_str());
  if (fuel.productCode.length() == 0 || fuel.tariffCode.length() == 0) return false;

  String url;
  if (fuel.isElectricity) {
    url = String(EDF_BASE_URL) + "/v1/products/" + fuel.productCode +
          "/electricity-tariffs/" + fuel.tariffCode +
          "/standing-charges/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=50";
  } else {
    url = String(EDF_BASE_URL) + "/v1/products/" + fuel.productCode +
          "/gas-tariffs/" + fuel.tariffCode +
          "/standing-charges/?period_from=" + isoUtc(from) +
          "&period_to=" + isoUtc(to) + "&page_size=50";
  }

  JsonDocument doc;
  if (!getJson(url, doc, authHeader(), fuel.isElectricity ? "electricity-standing-charge" : "gas-standing-charge")) {
    return false;
  }

  JsonArray results = doc["results"].as<JsonArray>();
  if (results.size() == 0) {
    *standingChargePence = 0;
    return true;
  }

  *standingChargePence = results[0]["value_inc_vat"] | 0.0f;
  LOGI("standing charge fuel=%s pence=%0.3f", fuel.isElectricity ? "electricity" : "gas", *standingChargePence);
  return true;
}

void addToBucket(Bucket& bucket, bool electricity, float kwh, float costPounds) {
  if (electricity) {
    bucket.electricityKwh += kwh;
    bucket.electricityCost += costPounds;
  } else {
    bucket.gasKwh += kwh;
    bucket.gasCost += costPounds;
  }
}

const char* consumptionGroupBy(Page page) {
  return page == PAGE_YEAR ? "month" : "day";
}

float consumptionKwhFor(const FuelConfig& fuel, float consumption) {
  if (fuel.isElectricity) return consumption;
  String units = fuel.consumptionUnits;
  units.toLowerCase();
  return units.indexOf("kwh") >= 0 ? consumption : convertGasM3ToKwh(consumption);
}

float standingChargeDaysFor(Page page, time_t slotStart, time_t windowFrom, time_t windowTo) {
  if (page == PAGE_YEAR) {
    struct tm tm {};
    localtime_r(&slotStart, &tm);
    time_t monthStart = localTimeAt(tm.tm_year + 1900, tm.tm_mon + 1, 1);
    time_t nextMonth = localTimeAt(tm.tm_year + 1900, tm.tm_mon + 2, 1);
    time_t chargeFrom = max(monthStart, windowFrom);
    time_t chargeTo = min(nextMonth, windowTo);
    return max(0.0f, static_cast<float>(chargeTo - chargeFrom) / 86400.0f);
  }
  return 1.0f;
}

void addStandingCharges(const FuelConfig& fuel, Page page, time_t from, time_t to, Bucket* buckets, int bucketCount, time_t pageStart, float standingChargePence) {
  if (standingChargePence <= 0) return;
  time_t bucketStart = pageStart != 0 ? pageStart : from;

  if (page == PAGE_YEAR) {
    struct tm start {};
    localtime_r(&bucketStart, &start);
    for (int i = 0; i < bucketCount; ++i) {
      time_t monthStart = localTimeAt(start.tm_year + 1900, start.tm_mon + 1 + i, 1);
      float days = standingChargeDaysFor(page, monthStart, from, to);
      if (days <= 0) continue;
      addToBucket(buckets[i], fuel.isElectricity, 0, (standingChargePence * days) / 100.0f);
    }
    return;
  }

  for (time_t day = from; day < to; day += 86400) {
    int index = bucketIndexFor(day, page, bucketStart);
    if (index < 0 || index >= bucketCount) continue;
    addToBucket(buckets[index], fuel.isElectricity, 0, standingChargePence / 100.0f);
  }
}

bool fetchFuelWindow(const FuelConfig& fuel, Page page, time_t from, time_t to, Bucket* buckets, int bucketCount, time_t pageStart = 0) {
  LOGI("fetchFuelWindow start fuel=%s present=%d page=%s from=%s to=%s bucket_count=%d",
       fuel.isElectricity ? "electricity" : "gas", fuel.present,
       pageNameC(page), isoUtc(from).c_str(), isoUtc(to).c_str(), bucketCount);
  if (!fuel.present) return true;
  if (!refreshAccessToken()) return false;

  JsonDocument ratesDoc;
  if (!fetchRates(fuel, from, to, ratesDoc)) return false;
  JsonArray rates = ratesDoc["results"].as<JsonArray>();

  float standingChargePence = 0;
  fetchStandingCharge(fuel, from, to, &standingChargePence);
  addStandingCharges(fuel, page, from, to, buckets, bucketCount, pageStart, standingChargePence);

  JsonDocument consumptionFilter;
  consumptionFilter["next"] = true;
  consumptionFilter["results"][0]["interval_start"] = true;
  consumptionFilter["results"][0]["start"] = true;
  consumptionFilter["results"][0]["consumption"] = true;

  const char* groupBy = consumptionGroupBy(page);
  LOGI("fetchFuelWindow aggregation fuel=%s group_by=%s gas_conversion=%s",
       fuel.isElectricity ? "electricity" : "gas", groupBy,
       fuel.isElectricity ? "none" : (fuel.consumptionUnits.c_str()));
  for (time_t sliceFrom = from; sliceFrom < to; sliceFrom = to) {
    time_t sliceTo = to;
    String url;
    if (fuel.isElectricity) {
      url = String(EDF_BASE_URL) + "/v1/electricity-meter-points/" + fuel.pointId +
            "/meters/" + fuel.serial + "/consumption/?period_from=" + isoUtc(sliceFrom) +
            "&period_to=" + isoUtc(sliceTo) + "&page_size=" + String(CONSUMPTION_PAGE_SIZE) +
            "&group_by=" + groupBy;
    } else {
      url = String(EDF_BASE_URL) + "/v1/gas-meter-points/" + fuel.pointId +
            "/meters/" + fuel.serial + "/consumption/?period_from=" + isoUtc(sliceFrom) +
            "&period_to=" + isoUtc(sliceTo) + "&page_size=" + String(CONSUMPTION_PAGE_SIZE) +
            "&group_by=" + groupBy;
    }

    while (url.length() > 0) {
      String nextUrl;
      JsonDocument doc;
      if (!getJson(url, doc, authHeader(), fuel.isElectricity ? "electricity-consumption" : "gas-consumption", &consumptionFilter)) {
        return false;
      }

      JsonArray results = doc["results"].as<JsonArray>();
      LOGI("fetchFuelWindow parsed data: %d items", results.size());

      int pageItems = 0;
      for (JsonObject item : results) {
        const char* startText = item["interval_start"];
        if (!startText) startText = item["start"];
        time_t slotStart = 0;
        if (!parseIsoTime(startText, &slotStart)) continue;
        time_t pStart = pageStart != 0 ? pageStart : from;
        int index = bucketIndexFor(slotStart, page, pStart);
        if (index < 0 || index >= bucketCount) continue;

        float consumption = item["consumption"] | 0.0f;
        float kwh = consumptionKwhFor(fuel, consumption);
        float pencePerKwh = rateAt(rates, slotStart);
        float costPounds = (kwh * pencePerKwh) / 100.0f;
        addToBucket(buckets[index], fuel.isElectricity, kwh, costPounds);
        pageItems++;
      }

      const char* next = doc["next"] | nullptr;
      LOGI("fetchFuelWindow grouped fuel=%s group_by=%s from=%s to=%s items=%d has_next=%d",
           fuel.isElectricity ? "electricity" : "gas", groupBy,
           isoUtc(sliceFrom).c_str(), isoUtc(sliceTo).c_str(), pageItems, next != nullptr);
      nextUrl = next == nullptr ? "" : String(next);
      doc.clear();
      url = nextUrl;
    }
  }

  LOGI("fetchFuelWindow complete fuel=%s page=%s", fuel.isElectricity ? "electricity" : "gas", pageNameC(page));
  return true;
}

void resetBuckets(Bucket* buckets, int count) {
  for (int i = 0; i < count; ++i) buckets[i] = Bucket {};
}

bool currentPageHasData() {
  int page = rtcCurrentPage;
  return page >= 0 && page < PAGE_COUNT && rtcPageHasData[page];
}

void setCurrentPageHasData(bool hasData) {
  int page = rtcCurrentPage;
  if (page >= 0 && page < PAGE_COUNT) rtcPageHasData[page] = hasData;
  rtcHasData = rtcPageHasData[PAGE_WEEK] || rtcPageHasData[PAGE_MONTH] || rtcPageHasData[PAGE_YEAR];
}

bool refreshData() {
  LOGI("refreshData start");
  ledPulse(255, 255, 255, 2);
  clearLastError();
  
  if (rtcCurrentPage == PAGE_WEEK) {
    resetBuckets(rtcWeek, 7);
  } else if (rtcCurrentPage == PAGE_MONTH) {
    resetBuckets(rtcMonth, MAX_MONTH_DAYS);
  } else if (rtcCurrentPage == PAGE_YEAR) {
    resetBuckets(rtcYear, YEAR_MONTHS);
  }
  
  rtcMonthDays = daysInCurrentMonth();

  time_t weekStart = startOfWeekLocal();
  time_t monthStart = startOfMonthLocal();
  time_t yearStart = startOfYearLocal();
  time_t nextWeek = weekStart + 7 * 86400;
  time_t nextMonth = monthStart + 30 * 86400;
  time_t nextYear;
  {
    struct tm tm {};
    localtime_r(&yearStart, &tm);
    nextYear = localTimeAt(tm.tm_year + 1900, tm.tm_mon + 1 + 12, 1);
  }
  LOGI("refresh windows week=%s..%s month=%s..%s year=%s..%s month_days=%d",
       isoUtc(weekStart).c_str(), isoUtc(nextWeek).c_str(),
       isoUtc(monthStart).c_str(), isoUtc(nextMonth).c_str(),
       isoUtc(yearStart).c_str(), isoUtc(nextYear).c_str(), rtcMonthDays);

  if (rtcCurrentPage == PAGE_WEEK) {
    if (!fetchFuelWindow(config.electricity, PAGE_WEEK, weekStart, nextWeek, rtcWeek, 7)) { setCurrentPageHasData(false); return false; }
    if (!fetchFuelWindow(config.gas, PAGE_WEEK, weekStart, nextWeek, rtcWeek, 7)) { setCurrentPageHasData(false); return false; }
  } else if (rtcCurrentPage == PAGE_MONTH) {
    if (!fetchFuelWindow(config.electricity, PAGE_MONTH, monthStart, nextMonth, rtcMonth, rtcMonthDays)) { setCurrentPageHasData(false); return false; }
    if (!fetchFuelWindow(config.gas, PAGE_MONTH, monthStart, nextMonth, rtcMonth, rtcMonthDays)) { setCurrentPageHasData(false); return false; }
  } else if (rtcCurrentPage == PAGE_YEAR) {
    if (!fetchFuelWindow(config.electricity, PAGE_YEAR, yearStart, nextYear, rtcYear, YEAR_MONTHS, yearStart)) { setCurrentPageHasData(false); return false; }
    if (!fetchFuelWindow(config.gas, PAGE_YEAR, yearStart, nextYear, rtcYear, YEAR_MONTHS, yearStart)) { setCurrentPageHasData(false); return false; }
  }

  setCurrentPageHasData(true);
  rtcLastRefresh = nowUtc();
  clearLastError();
  ledPulse(0, 255, 0, 2);
  LOGI("refreshData complete last_refresh=%ld", static_cast<long>(rtcLastRefresh));
  return true;
}

String pageName(Page page) {
  return String(pageNameC(page));
}
