// ESP32-WROOM weather display
// Board:   ESP32 Dev Module (ESP32-WROOM-32)
// Display: 1.98" 128x64 OLED (SSD1309 / SSD1306), I2C
// Weather: Open-Meteo (free, no API key) for Jonesboro, AR
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

const uint32_t REFRESH_MS = 10UL * 60UL * 1000UL;  // update every 10 minutes
const uint32_t WIFI_TIMEOUT_MS = 20000;

// The top line rotates between the city name and the next three days' forecast.
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
  float temp = 0, feelsLike = 0, wind = 0, high = 0, low = 0;
  int   humidity = 0;
  int   code = 0;
};

struct ForecastDay {
  char name[8] = "";
  int  code = 0;
  int  high = 0, low = 0;
};

Weather weather;
ForecastDay forecast[3];
bool forecastValid = false;
bool lastFetchFailed = false;
uint32_t lastFetch = 0;

const char* describeCode(int code) {
  if (code == 0)  return "Clear";
  if (code == 1)  return "Mst clear";
  if (code == 2)  return "Pt cloudy";
  if (code == 3)  return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code == 85 || code == 86) return "Flurries";
  if (code >= 95)               return "Storms";
  return "Unknown";
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

bool fetchWeather() {
  if (!connectWiFi()) return false;

  String url = String("https://api.open-meteo.com/v1/forecast")
             + "?latitude=" + String(LATITUDE, 4)
             + "&longitude=" + String(LONGITUDE, 4)
             + "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
               "weather_code,wind_speed_10m"
             + "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             + "&temperature_unit=fahrenheit&wind_speed_unit=mph"
             + "&timezone=America%2FChicago&forecast_days=4";

  WiFiClientSecure client;
  client.setInsecure();  // skips certificate check; fine for public weather data

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);  // avoid chunked encoding, which breaks streaming JSON parsing
  if (!http.begin(client, url)) return false;

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d\n", status);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return false;
  }

  JsonObject cur = doc["current"];
  weather.temp      = cur["temperature_2m"] | 0.0f;
  weather.feelsLike = cur["apparent_temperature"] | 0.0f;
  weather.humidity  = cur["relative_humidity_2m"] | 0;
  weather.wind      = cur["wind_speed_10m"] | 0.0f;
  weather.code      = cur["weather_code"] | 0;
  weather.high      = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  weather.low       = doc["daily"]["temperature_2m_min"][0] | 0.0f;
  weather.valid     = true;

  // Next 3 days (today is index 0 and is already on screen). Day names come
  // from the dates ("2026-09-22" -> "Tue").
  JsonObject daily = doc["daily"];
  for (int i = 0; i < 3; i++) {
    ForecastDay& d = forecast[i];
    int idx = i + 1;
    const char* date = daily["time"][idx] | "";
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
    d.code = daily["weather_code"][idx] | 0;
    d.high = (int)roundf(daily["temperature_2m_max"][idx] | 0.0f);
    d.low  = (int)roundf(daily["temperature_2m_min"][idx] | 0.0f);
  }
  forecastValid = !daily["time"][3].isNull();
  for (int i = 0; i < 3 && forecastValid; i++) {
    Serial.printf("  %s %d/%d %s\n", forecast[i].name, forecast[i].high, forecast[i].low,
                  describeCode(forecast[i].code));
  }

  Serial.printf("%.0fF, %s\n", weather.temp, describeCode(weather.code));
  return true;
}

void drawDegree(int x, int y) {
  u8g2.drawCircle(x, y, 2);
}

// Top-line text: cycles through the city name and the next three days,
// e.g. "Tue 93/74 Rain". The condition is dropped if it won't fit before the Wi-Fi bars.
void drawHeaderText() {
  const char* text = CITY_NAME;
  char buf[24];
  uint32_t slot = (millis() / 1000 / HEADER_SLOT_SECS) % 4;
  if (forecastValid && slot > 0) {
    const ForecastDay& d = forecast[slot - 1];
    snprintf(buf, sizeof(buf), "%s %d/%d %s", d.name, d.high, d.low, describeCode(d.code));
    if (u8g2.getStrWidth(buf) > 96) snprintf(buf, sizeof(buf), "%s %d/%d", d.name, d.high, d.low);
    text = buf;
  }
  u8g2.drawStr(0, 8, text);
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

  // Header
  u8g2.setFont(u8g2_font_6x10_tr);
  drawHeaderText();
  drawWiFiBars();
  if (lastFetchFailed) u8g2.drawStr(122, 8, "!");  // last refresh failed, showing old data
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
  const char* cond = describeCode(weather.code);
  int cw = u8g2.getStrWidth(cond);
  u8g2.drawStr(max(0, 128 - cw), 24, cond);

  // Details
  snprintf(buf, sizeof(buf), "Feels %d  Hum %d%%",
           (int)roundf(weather.feelsLike), weather.humidity);
  u8g2.drawStr(0, 52, buf);
  snprintf(buf, sizeof(buf), "H%d L%d  Wind %dmph",
           (int)roundf(weather.high), (int)roundf(weather.low),
           (int)roundf(weather.wind));
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
  lastFetchFailed = !fetchWeather();
  lastFetch = millis();
}

void loop() {
  // Retry sooner (1 min) after a failure, otherwise use the normal interval.
  uint32_t interval = lastFetchFailed ? 60UL * 1000UL : REFRESH_MS;

  if (millis() - lastFetch >= interval) {
    lastFetchFailed = !fetchWeather();
    lastFetch = millis();
  }

  if (weather.valid) {
    drawWeather();
  } else {
    showMessage("No weather yet", "Retrying in 1 min");
  }
  delay(1000);
}
