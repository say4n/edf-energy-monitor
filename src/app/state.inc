constexpr const char* EDF_BASE_URL = "https://api.edfgb-kraken.energy";
constexpr const char* GRAPHQL_URL = "https://api.edfgb-kraken.energy/v1/graphql/";
constexpr const char* TZ_LONDON = "GMT0BST,M3.5.0/1,M10.5.0/2";
constexpr const char *EDF_USER_AGENT = "say4n/edf-energy-monitor";
constexpr const char* EDF_CONTEXT_HEADER = "Ha-Integration-Context";

constexpr gpio_num_t BTN_UP_PIN = GPIO_NUM_10;      // G10
constexpr gpio_num_t BTN_DOWN_PIN = GPIO_NUM_9;     // G9
constexpr gpio_num_t BTN_REFRESH_PIN = GPIO_NUM_1;  // G1
constexpr uint64_t ONE_HOUR_US = 60ULL * 60ULL * 1000000ULL;
constexpr int MAX_MONTH_DAYS = 31;
constexpr int YEAR_MONTHS = 12;
constexpr int CONSUMPTION_PAGE_SIZE = 64;
constexpr uint32_t RTC_STATE_VERSION = 2;
constexpr uint16_t COLOR_ELECTRICITY = 0x001F;  // blue
constexpr uint16_t COLOR_GAS = 0xFBE0;          // orange/yellow
constexpr uint16_t COLOR_HEADER = 0x001F;
constexpr uint16_t COLOR_SUCCESS = 0x07E0;
constexpr uint16_t COLOR_WARNING = 0xFBE0;
constexpr uint16_t COLOR_GRID = 0xBDF7;
constexpr uint16_t COLOR_MUTED = 0x7BEF;
constexpr uint16_t COLOR_CHART_LABEL = 0x39E7;
constexpr uint16_t COLOR_PANEL = 0xFFFF;
constexpr uint8_t PAPER_COLOR_LED_PIN = 21;
constexpr uint8_t PAPER_COLOR_LED_COUNT = 2;

enum Page : int {
  PAGE_WEEK = 0,
  PAGE_MONTH = 1,
  PAGE_YEAR = 2,
  PAGE_COUNT = 3,
};

const char* pageNameC(Page page) {
  switch (page) {
    case PAGE_WEEK: return "Week";
    case PAGE_MONTH: return "Month";
    case PAGE_YEAR: return "Year";
    default: return "Unknown";
  }
}

struct Bucket {
  float electricityKwh = 0;
  float gasKwh = 0;
  float electricityCost = 0;
  float gasCost = 0;
};

struct FuelConfig {
  bool present = false;
  bool isElectricity = false;
  bool smartMeter = true;
  String pointId;
  String serial;
  String productCode;
  String tariffCode;
  String consumptionUnits;
  float standingChargePence = 0;
};

struct AppConfig {
  bool configured = false;
  String accountNumber;
  String refreshToken;
  FuelConfig electricity;
  FuelConfig gas;
};

struct TokenState {
  String accessToken;
  String refreshToken;
  time_t accessExpiresAt = 0;
  time_t refreshExpiresAt = 0;
};

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

bool connectWifi();
bool ensureWifiConnected(const char* context);
