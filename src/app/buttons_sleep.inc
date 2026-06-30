bool buttonLow(gpio_num_t pin) {
  return digitalRead(static_cast<uint8_t>(pin)) == LOW;
}

uint64_t ext1WakeMask() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
}

bool wakePinTriggered(gpio_num_t pin) {
  return (ext1WakeMask() & (1ULL << pin)) != 0;
}

bool refreshButtonWake() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  uint64_t mask = ext1WakeMask();
  LOGI("refreshButtonWake wake_cause=%d wake_mask=0x%llx refresh_triggered=%d refresh_pin_low=%d",
       cause, static_cast<unsigned long long>(mask), wakePinTriggered(BTN_REFRESH_PIN), buttonLow(BTN_REFRESH_PIN));
  if (cause == ESP_SLEEP_WAKEUP_TIMER) return true;
  if (cause != ESP_SLEEP_WAKEUP_EXT1) return false;
  return wakePinTriggered(BTN_REFRESH_PIN);
}

bool applyPageButtonWake() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  uint64_t mask = ext1WakeMask();
  bool upWake = wakePinTriggered(BTN_UP_PIN);
  bool downWake = wakePinTriggered(BTN_DOWN_PIN);
  LOGI("applyPageButtonWake wake_cause=%d wake_mask=0x%llx up_wake=%d down_wake=%d up_low=%d down_low=%d current_page=%s",
       cause, static_cast<unsigned long long>(mask), upWake, downWake,
       buttonLow(BTN_UP_PIN), buttonLow(BTN_DOWN_PIN), pageNameC(static_cast<Page>(rtcCurrentPage)));
  if (cause != ESP_SLEEP_WAKEUP_EXT1) return false;

  if (upWake) {
    rtcCurrentPage = (rtcCurrentPage + PAGE_COUNT - 1) % PAGE_COUNT;
    LOGI("wake page changed via up to %s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    return true;
  } else if (downWake) {
    rtcCurrentPage = (rtcCurrentPage + 1) % PAGE_COUNT;
    LOGI("wake page changed via down to %s", pageNameC(static_cast<Page>(rtcCurrentPage)));
    return true;
  }
  return false;
}

void prepareButtons() {
  LOGI("configuring buttons up=%d down=%d refresh=%d", BTN_UP_PIN, BTN_DOWN_PIN, BTN_REFRESH_PIN);
  pinMode(static_cast<uint8_t>(BTN_UP_PIN), INPUT_PULLUP);
  pinMode(static_cast<uint8_t>(BTN_DOWN_PIN), INPUT_PULLUP);
  pinMode(static_cast<uint8_t>(BTN_REFRESH_PIN), INPUT_PULLUP);
}

void sleepUntilNextWake() {
  LOGI("Entering deep sleep page=%s has_data=%d last_refresh=%ld",
       pageNameC(static_cast<Page>(rtcCurrentPage)), rtcHasData, static_cast<long>(rtcLastRefresh));
  ledPulse(0, 0, 255, 1);
  ledOff();
  delay(200);

  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  for (gpio_num_t pin : {BTN_UP_PIN, BTN_DOWN_PIN, BTN_REFRESH_PIN}) {
    rtc_gpio_init(pin);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(pin);
    rtc_gpio_pullup_en(pin);
  }

  esp_sleep_enable_ext1_wakeup(
    (1ULL << BTN_UP_PIN) | (1ULL << BTN_DOWN_PIN) | (1ULL << BTN_REFRESH_PIN),
    ESP_EXT1_WAKEUP_ANY_LOW
  );
  esp_sleep_enable_timer_wakeup(ONE_HOUR_US);
  esp_deep_sleep_start();
}
