// ESP32-WROOM weather display
// Board:   ESP32 Dev Module (ESP32-WROOM-32)
// Display: 1.98" 128x64 OLED (SSD1309 / SSD1306), I2C
// Weather: National Weather Service (api.weather.gov), free, no API key,
//          for Jonesboro, AR. Also polls for active severe weather alerts
//          (watches/warnings) and flashes them across the top of the screen.
//
// Libraries (installed automatically by PlatformIO, see platformio.ini):
//   - U8g2 by olikraus
//   - ArduinoJson by Benoit Blanchon (version 7.x)
//
// Wiring:
//   OLED VCC -> 3V3    OLED GND -> GND
//   OLED SDA -> GPIO21 OLED SCL -> GPIO22

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>

// ---------- Settings you edit ----------
#include "secrets.h"  // WIFI_SSID and WIFI_PASSWORD (kept out of GitHub)

const char* CITY_NAME = "JONESBORO, AR";
const float LATITUDE  = 35.8423;
const float LONGITUDE = -90.7043;

// National Weather Service identifiers for the coordinates above. These were
// looked up once from https://api.weather.gov/points/{lat},{lon} and are
// hardcoded here so the device doesn't need an extra lookup call on every
// boot. If you change LATITUDE/LONGITUDE, look these up again the same way.
const char* NWS_USER_AGENT = "esp32-weather-display (github.com/booherjr707-code/esp32-weather-display)";
const char* NWS_GRID_ID = "MEG";
const int   NWS_GRID_X  = 17;
const int   NWS_GRID_Y  = 97;
const char* NWS_STATION = "KJBR";  // Jonesboro Municipal Airport

const uint32_t WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;  // conditions + forecast
const uint32_t ALERT_REFRESH_MS   = 2UL  * 60UL * 1000UL;  // severe weather alerts, checked more often
const uint32_t WIFI_TIMEOUT_MS = 20000;

// The top line rotates between the city name and the next three days' forecast.
// While a severe alert is active, this is replaced by a flashing alert banner.
const uint32_t HEADER_SLOT_SECS = 3;  // seconds each item stays up

// US Central time, switches to daylight saving automatically.
const char* TIMEZONE = "CST6CDT,M3.2.0,M11.1.0";

// Pick ONE display line. If the screen stays blank or looks shifted,
// swap to the other one (SSD1306 and SSD1309 are nearly identical).
// U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Most modules answer at 0x3C. If yours is 0x3D, change this.
const uint8_t OLED_I2C_ADDR = 0x3C;
// ---------------------------------------

struct Weather {
  bool  valid = false;
  float temp = 0, feelsLike = 0, wind = 0;
  int   high = 0, low = 0;
  int   humidity = 0;
  char  condition[24] = "";  // from the live station observation, e.g. "Partly Cloudy"
};

struct ForecastDay {
  char name[8] = "";
  char cond[20] = "";
  int  high = 0, low = 0;
};

struct Alert {
  bool active = false;
  int  severityRank = 0;   // 0 none, 1 unknown/minor .. 5 extreme
  char event[36] = "";     // e.g. "Severe Thunderstorm Warning"
};

Weather weather;
ForecastDay forecast[3];
Alert alert;
bool forecastValid = false;
bool lastFetchFailed = false;
bool lastAlertFetchFailed = false;
uint32_t lastFetch = 0;
uint32_t lastAlertFetch = 0;

// Higher = more urgent. Anything present (even "Unknown") is worth flagging.
int severityRank(const char* sev) {
  if (strcmp(sev, "Extreme")  == 0) return 5;
  if (strcmp(sev, "Severe")   == 0) return 4;
  if (strcmp(sev, "Moderate") == 0) return 3;
  if (strcmp(sev, "Minor")    == 0) return 2;
  return 1;
}

// Phone-style signal bars (0-4) from the Wi-Fi RSSI in dBm.
int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

// Draws 4 bars in the top-right of the header. Lit bars are solid;
// unlit bars are just a thin line along the bottom, like a phone.
void drawWiFiBars() {
  int bars = wifiBars();
  for (int i = 0; i < 4; i++) {
    int x = 100 + i * 5;
    int h = 3 + i * 2;  // heights 3, 5, 7, 9
    if (i < bars) u8g2.drawBox(x, 10 - h, 3, h);
    else          u8g2.drawHLine(x, 9, 3);
  }
}

void showMessage(const char* line1, const char* line2 = "") {
  u8g2.clearBuffer();
  drawWiFiBars();
  u8g2.setFont(u8g2_font_7x13B_tr);
  u8g2.drawStr(0, 26, line1);
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 44, line2);
  u8g2.sendBuffer();
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  showMessage("Connecting WiFi", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Fetches live current conditions from the nearest NWS observation station.
// NWS reports temperature/wind/humidity in metric regardless of locale, so
// we convert to F/mph here.
bool fetchConditions() {
  if (!connectWiFi()) return false;

  String url = String("https://api.weather.gov/stations/") + NWS_STATION + "/observations/latest";

  WiFiClientSecure client;
  client.setInsecure();  // skips certificate check; fine for public weather data

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);  // avoid chunked encoding, which breaks streaming JSON parsing
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", NWS_USER_AGENT);  // api.weather.gov asks every client to identify itself

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Conditions HTTP error: %d\n", status);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Conditions JSON error: %s\n", err.c_str());
    return false;
  }

  JsonObject p = doc["properties"];
  float tempC = p["temperature"]["value"] | NAN;
  if (isnan(tempC)) return false;  // station has no current reading yet

  float feelsC = p["heatIndex"]["value"] | NAN;
  if (isnan(feelsC)) feelsC = p["windChill"]["value"] | NAN;
  if (isnan(feelsC)) feelsC = tempC;

  float windKmh   = p["windSpeed"]["value"] | 0.0f;
  float humidity  = p["relativeHumidity"]["value"] | 0.0f;
  const char* desc = p["textDescription"] | "Unknown";

  weather.temp      = tempC * 9.0f / 5.0f + 32.0f;
  weather.feelsLike = feelsC * 9.0f / 5.0f + 32.0f;
  weather.humidity  = (int)roundf(humidity);
  weather.wind      = windKmh * 0.621371f;
  strlcpy(weather.condition, desc, sizeof(weather.condition));
  weather.valid = true;

  Serial.printf("%.0fF, %s\n", weather.temp, weather.condition);
  return true;
}

// Fetches today's high/low plus the next 3 days from the NWS gridpoint
// forecast. Each day has a daytime and nighttime period; we pair them up by
// date to get one high/low/condition per day.
bool fetchForecast() {
  if (!connectWiFi()) return false;

  String url = String("https://api.weather.gov/gridpoints/") + NWS_GRID_ID + "/" +
               NWS_GRID_X + "," + NWS_GRID_Y + "/forecast";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", NWS_USER_AGENT);

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Forecast HTTP error: %d\n", status);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Forecast JSON error: %s\n", err.c_str());
    return false;
  }

  JsonArray periods = doc["properties"]["periods"];

  bool gotHigh = false, gotLow = false;
  int dayCount = -1;         // -1 before first period; 0 = today; 1..3 = the header days
  char lastDate[11] = "";

  for (JsonObject p : periods) {
    const char* start = p["startTime"] | "";
    char date[11] = "";
    strncpy(date, start, 10);
    date[10] = 0;
    bool isDay   = p["isDaytime"] | false;
    int  temp    = p["temperature"] | 0;
    const char* cond = p["shortForecast"] | "";

    // Today's high is the first daytime reading we see; tonight's low is the
    // first nighttime reading, regardless of which "day" they fall under.
    if (!gotHigh && isDay)  { weather.high = temp; gotHigh = true; }
    if (!gotLow  && !isDay) { weather.low  = temp; gotLow  = true; }

    if (strcmp(date, lastDate) != 0) {
      dayCount++;
      strlcpy(lastDate, date, sizeof(lastDate));
      if (dayCount > 3) break;
      if (dayCount >= 1) {
        ForecastDay& d = forecast[dayCount - 1];
        d.high = 0;
        d.low  = 0;
        d.cond[0] = 0;
        struct tm t = {};
        int y, m, day;
        if (sscanf(date, "%d-%d-%d", &y, &m, &day) == 3) {
          t.tm_year = y - 1900;
          t.tm_mon  = m - 1;
          t.tm_mday = day;
          t.tm_hour = 12;
          mktime(&t);  // fills in the weekday
          strftime(d.name, sizeof(d.name), "%a", &t);
        } else {
          strlcpy(d.name, "---", sizeof(d.name));
        }
      }
    }

    if (dayCount >= 1 && dayCount <= 3) {
      ForecastDay& d = forecast[dayCount - 1];
      if (isDay) {
        d.high = temp;
        strlcpy(d.cond, cond, sizeof(d.cond));
      } else {
        d.low = temp;
        if (!d.cond[0]) strlcpy(d.cond, cond, sizeof(d.cond));
      }
    }
  }

  forecastValid = (dayCount >= 3);
  for (int i = 0; i < 3 && forecastValid; i++) {
    Serial.printf("  %s %d/%d %s\n", forecast[i].name, forecast[i].high, forecast[i].low, forecast[i].cond);
  }

  return true;
}

// Fetches active NWS alerts (watches/warnings/advisories) for this point and
// keeps the most urgent one. An empty result means no active alerts.
bool fetchAlerts() {
  if (!connectWiFi()) return false;

  String url = String("https://api.weather.gov/alerts/active?point=") +
               String(LATITUDE, 4) + "," + String(LONGITUDE, 4);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", NWS_USER_AGENT);

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Alert HTTP error: %d\n", status);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Alert JSON error: %s\n", err.c_str());
    return false;
  }

  JsonArray features = doc["features"];
  int bestRank = 0;
  const char* bestEvent = "Alert";
  for (JsonObject f : features) {
    JsonObject p = f["properties"];
    int rank = severityRank(p["severity"] | "Unknown");
    if (rank > bestRank) {
      bestRank = rank;
      bestEvent = p["event"] | "Alert";
    }
  }

  bool wasActive = alert.active;
  alert.active = bestRank > 0;
  alert.severityRank = bestRank;
  if (alert.active) {
    strlcpy(alert.event, bestEvent, sizeof(alert.event));
    if (!wasActive) Serial.printf("ALERT: %s\n", alert.event);
  } else {
    alert.event[0] = 0;
    if (wasActive) Serial.println("Alert cleared");
  }
  return true;
}

void drawDegree(int x, int y) {
  u8g2.drawCircle(x, y, 2);
}

// Top-line text: cycles through the city name and the next three days,
// e.g. "Tue 93/74 Sunny". The condition is dropped if it won't fit before the Wi-Fi bars.
void drawHeaderText() {
  const char* text = CITY_NAME;
  char buf[28];
  uint32_t slot = (millis() / 1000 / HEADER_SLOT_SECS) % 4;
  if (forecastValid && slot > 0) {
    const ForecastDay& d = forecast[slot - 1];
    snprintf(buf, sizeof(buf), "%s %d/%d %s", d.name, d.high, d.low, d.cond);
    if (u8g2.getStrWidth(buf) > 96) snprintf(buf, sizeof(buf), "%s %d/%d", d.name, d.high, d.low);
    text = buf;
  }
  u8g2.drawStr(0, 8, text);
}

// Scrolls text leftward if it's wider than the box; otherwise just draws it.
void drawMarquee(int y, const char* text, int boxW) {
  int textW = u8g2.getStrWidth(text);
  if (textW <= boxW) {
    u8g2.drawStr(0, y, text);
    return;
  }
  const int gap = 20;
  int totalW = textW + gap;
  int offset = (millis() / 40) % totalW;
  int x = -offset;
  u8g2.drawStr(x, y, text);
  if (x + totalW < boxW) u8g2.drawStr(x + totalW, y, text);
}

// Replaces the header with a blinking, inverted banner naming the most
// urgent active alert (e.g. "!! SEVERE THUNDERSTORM WARNING"), scrolling it
// if it's too wide to fit.
void drawAlertBanner() {
  bool flash = (millis() / 500) % 2 == 0;
  u8g2.setClipWindow(0, 0, 127, 11);
  u8g2.drawBox(0, 0, 128, 11);
  u8g2.setDrawColor(0);  // black on the white banner
  char banner[40];
  snprintf(banner, sizeof(banner), "%s %s", flash ? "!!" : "  ", alert.event);
  drawMarquee(9, banner, 128);
  u8g2.setDrawColor(1);
  u8g2.setMaxClipWindow();
}

// Small 12-hour clock, right-aligned under the condition text.
// Shows "--:--" until the first internet time sync arrives.
void drawClock() {
  char buf[12];
  struct tm t;
  if (getLocalTime(&t, 0)) {
    int h = t.tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  } else {
    snprintf(buf, sizeof(buf), "--:--");
  }
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(128 - u8g2.getStrWidth(buf), 38, buf);
}

void drawWeather() {
  char buf[32];
  u8g2.clearBuffer();

  // Header: normally rotates city/forecast, but a severe alert takes over.
  u8g2.setFont(u8g2_font_6x10_tr);
  if (alert.active) {
    drawAlertBanner();
  } else {
    drawHeaderText();
    drawWiFiBars();
    if (lastFetchFailed) u8g2.drawStr(122, 8, "!");  // last refresh failed, showing old data
  }
  u8g2.drawHLine(0, 11, 128);

  // Big temperature
  u8g2.setFont(u8g2_font_logisoso24_tr);
  snprintf(buf, sizeof(buf), "%d", (int)roundf(weather.temp));
  int w = u8g2.drawStr(0, 40, buf);
  drawDegree(w + 5, 20);
  u8g2.setFont(u8g2_font_logisoso16_tr);
  u8g2.drawStr(w + 10, 40, "F");

  // Condition, right of the temperature
  u8g2.setFont(u8g2_font_6x10_tr);
  const char* cond = weather.condition;
  int cw = u8g2.getStrWidth(cond);
  u8g2.drawStr(max(0, 128 - cw), 24, cond);

  // Details
  snprintf(buf, sizeof(buf), "Feels %d  Hum %d%%",
           (int)roundf(weather.feelsLike), weather.humidity);
  u8g2.drawStr(0, 52, buf);
  snprintf(buf, sizeof(buf), "H%d L%d  Wind %dmph",
           weather.high, weather.low, (int)roundf(weather.wind));
  u8g2.drawStr(0, 63, buf);

  drawClock();
  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  u8g2.setI2CAddress(OLED_I2C_ADDR << 1);
  u8g2.begin();

  showMessage("Weather Station", "Starting...");
  delay(800);

  if (!connectWiFi()) {
    showMessage("WiFi failed", "Check SSID/password");
    delay(3000);
  } else {
    showMessage("WiFi connected", WiFi.localIP().toString().c_str());
    delay(1000);
  }

  configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");  // start the clock sync

  showMessage("Getting weather...");
  bool condOk = fetchConditions();
  bool fcOk   = fetchForecast();
  lastFetchFailed = !(condOk && fcOk);
  lastFetch = millis();

  lastAlertFetchFailed = !fetchAlerts();
  lastAlertFetch = millis();
}

void loop() {
  // Retry sooner (1 min) after a failure, otherwise use the normal interval.
  uint32_t interval = lastFetchFailed ? 60UL * 1000UL : WEATHER_REFRESH_MS;
  if (millis() - lastFetch >= interval) {
    bool condOk = fetchConditions();
    bool fcOk   = fetchForecast();
    lastFetchFailed = !(condOk && fcOk);
    lastFetch = millis();
  }

  // Alerts are checked more often, and retried sooner (30s) after a failure,
  // since severe weather can develop quickly.
  uint32_t alertInterval = lastAlertFetchFailed ? 30UL * 1000UL : ALERT_REFRESH_MS;
  if (millis() - lastAlertFetch >= alertInterval) {
    lastAlertFetchFailed = !fetchAlerts();
    lastAlertFetch = millis();
  }

  if (weather.valid) {
    drawWeather();
  } else {
    showMessage("No weather yet", "Retrying in 1 min");
  }
  // Redraw faster while an alert is active so the flash/scroll look smooth.
  delay(alert.active ? 200 : 1000);
}
