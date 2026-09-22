# ESP32 Weather Display: Next Steps

**Status (2026-09-21):** Finished and running on real hardware.

## When the boards arrive
1. Wire the OLED to the ESP32:
   - VCC to 3V3
   - GND to GND
   - SDA to GPIO21
   - SCL to GPIO22
2. Copy `src/secrets.example.h` to `src/secrets.h` and fill in your Wi-Fi name and password. The ESP32 needs a 2.4 GHz network.
3. Plug the ESP32 in over USB.
4. In VS Code, click the PlatformIO alien icon, then **Upload and Monitor**.
   Or from this folder in a terminal:
   `~/.platformio/penv/bin/pio run -t upload -t monitor`

## If something is wrong
| Symptom | Fix |
|---|---|
| Blank screen | Swap the display line in `main.cpp` between `SSD1306` (the one that works on the author's display) and the commented `SSD1309` line |
| Blank screen, still | Change `OLED_I2C_ADDR` from `0x3C` to `0x3D` and check the SDA/SCL wires |
| "WiFi failed" | Recheck the SSID and password. The ESP32 only works on 2.4 GHz Wi-Fi. |
| Upload can't find the board | Try another USB cable (it must carry data). Hold the BOOT button while uploading starts. |
| Shows `!` in the corner | The last refresh failed and old data is showing. Check the serial monitor. |

## Ideas for later
- (Done) 3-day outlook: the top line rotates through the city and the next 3 days
- (Done) Clock: small 12-hour NTP clock, US Central time
- (Done) Switched weather source from Open-Meteo to the National Weather Service, and
  added severe weather alerts (flashing banner) — 2026-09-22
- Real lightning proximity (not just "a severe t-storm warning is active"): would need
  an AS3935 lightning detector sensor (~$8-10, I2C) wired in alongside the OLED. James
  doesn't have one yet.
- Button to cycle between screens
- Add `pio` to your shell PATH
