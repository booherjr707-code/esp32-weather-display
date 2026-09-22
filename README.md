# ESP32 Weather Display

A small weather station for an ESP32 and a 128x64 I2C OLED. It gets the weather for
Jonesboro, AR from the [National Weather Service](https://www.weather.gov/documentation/services-web-api)
(`api.weather.gov`, free, no API key) and shows:

- current temperature, conditions, feels-like, humidity and wind (from the nearest NWS
  observation station)
- today's high and low, and a top line that rotates between the city name and the next
  3 days' forecast
- a small 12-hour clock (US Central time, syncs over the internet)
- a 4-bar Wi-Fi signal indicator

Conditions and the forecast refresh every 10 minutes. Active **NWS severe weather
alerts** (watches, warnings, advisories) are checked every 2 minutes; when one is
active, it takes over the top line as a flashing, scrolling banner (e.g. "!! SEVERE
THUNDERSTORM WARNING") until it clears.

## Hardware

- ESP32-WROOM-32 dev board
- 1.98" 128x64 monochrome OLED, I2C, 4 pins (address 0x3C, SSD1306 driver)

| OLED | ESP32 |
|------|-------|
| VCC  | 3V3   |
| GND  | GND   |
| SDA  | GPIO21 |
| SCL  | GPIO22 |

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Copy `src/secrets.example.h` to `src/secrets.h` and fill in your Wi-Fi name and
   password. `secrets.h` is git-ignored so your password is never uploaded. The ESP32
   only works on 2.4 GHz Wi-Fi.
3. Change the city, latitude and longitude near the top of `src/main.cpp` if you are
   not in Jonesboro. If you do, also look up your new `NWS_GRID_ID`/`NWS_GRID_X`/
   `NWS_GRID_Y`/`NWS_STATION` from `https://api.weather.gov/points/{lat},{lon}` and
   its `observationStations` link, and update those constants too.
4. Plug in the ESP32 with a USB cable that carries data, then run
   `pio run -t upload -t monitor`.

If the screen stays blank, try the SSD1309 display line in `src/main.cpp` instead of
SSD1306, or change the I2C address from 0x3C to 0x3D. See `NEXT_STEPS.md` for more
troubleshooting.

## Credits

Built with [Claude Code](https://claude.com/claude-code), Anthropic's AI coding assistant, as a first ESP32 hardware project.
