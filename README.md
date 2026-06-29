# ESP32-RLCD-Apps

> Bilingual: [English](README.md) | [中文](README_zh.md)

A multi-app firmware for the **Waveshare ESP32-S3-RLCD-4.2** development board — a 4.2-inch reflective LCD dashboard that runs six independent applications switchable from an on-screen menu.

Built on ESP-IDF v6.0.1. Tested on ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM).

## Apps

| App | Description | Status |
|-----|-------------|--------|
| **MENU** | 5-item vertical launcher with cursor navigation | ✅ Verified |
| **Calendar** | Solar + lunar (1901–2050) monthly view, real weather (Open-Meteo), Feishu CalDAV events, indoor temp/humidity (SHTC3), 3-key day navigation | ✅ Verified |
| **XiaoZhi** | AI voice assistant over WebSocket (XiaoZhi public server), Opus codec, ES8311/ES7210 audio | ✅ Verified |
| **CodePilot** | Claude/Kimi status dashboard, receives data from PC bridge over WebSocket (port 7897), 3 views (split / detail / notification), long-press GPIO18 triggers voice-to-text via XiaoZhi STT (speaker muted) | ⏳ Code complete, hardware verification pending |
| **Snake** | Classic Nokia-style snake, relative-turn controls, progressive speed-up | ⏳ Code complete, hardware verification pending |
| **Tetris** | Standard 10×20 Tetris, 7 pieces × 4 rotations, hard-drop via long-press | ⏳ Code complete, hardware verification pending |

## Hardware

- **Board**: [Waveshare ESP32-S3-RLCD-4.2](https://www.waveshare.com/wiki/ESP32-S3-RLCD-4.2)
- **Display**: ST7306 400×300 monochrome reflective LCD (SPI)
- **Audio**: ES8311 (speaker) + ES7210 (4-channel mic) over I2S
- **Sensors**: SHTC3 temperature/humidity (I²C @ 0x70, shared with codecs)
- **Buttons**: BOOT (GPIO0), KEY (GPIO18), plus a 3-key keyboard (GPIO1/3/17) connected via the 2×8 header

### GPIO map

| Function | GPIO |
|----------|------|
| SPI CLK / MOSI / CS / DC / RST | 11 / 12 / 40 / 5 / 41 |
| I2S MCLK / BCLK / WS / DIN / DOUT | 16 / 9 / 45 / 10 / 8 |
| Speaker amp enable | 46 |
| I²C SDA / SCL (codecs + SHTC3) | 13 / 14 |
| Battery ADC (3× divider) | 2 |
| BOOT (XiaoZhi voice trigger) | 0 |
| PREV / NEXT / ENTER (external keyboard) | 1 / 3 / 17 |
| KEY (long-press, voice input in CodePilot) | 18 |

> GPIO43 was originally UART0 TX. The firmware redirects console output to USB-Serial/JTAG, freeing GPIO43. Since the build has no physical button on GPIO43, GPIO18 short-press is aliased to BACK at the dispatcher level.

## Build & flash

Requires ESP-IDF v6.0.1 (or compatible).

```bash
# Activate ESP-IDF environment
source ~/.espressif/tools/activate_idf_v6.0.1.sh

# Build
idf.py build

# Flash (adjust port as needed)
idf.py -p /dev/cu.usbmodem21201 flash

# Monitor
idf.py -p /dev/cu.usbmodem21201 monitor
```

### First-boot WiFi setup

If no WiFi credentials are saved, the device starts an AP config mode. Connect to the AP and follow the captive portal to set SSID/password. On subsequent boots, the device auto-connects as a station.

### Console output

Logs go through USB-Serial/JTAG (the same USB port used for flashing). No external UART hookup needed.

## Project structure

```
.
├── main/
│   ├── hello_world_main.c         Entry point + global state
│   ├── app_manager.c              Dispatcher + cooperative lifecycle
│   ├── app_framework.h            app_t interface, app_id_t enum
│   ├── app_registry.c             Static registration of 6 apps
│   ├── menu_app.c                 5-item menu UI
│   ├── calendar_app.c             Calendar worker (weather/CalDAV/SHTC3)
│   ├── calendar.c                 Calendar rendering library
│   ├── xiaozhi_app.c              XiaoZhi wrapper (lifecycle)
│   ├── xiaozhi_app_display.c      XiaoZhi UI + async display queue
│   ├── xiaozhi_display.c          Text/state callback router
│   ├── codepilot_app.c            CodePilot 3-view dashboard
│   ├── snake_app.c                Snake game
│   ├── tetris_app.c               Tetris game
│   ├── placeholder_app.c          Generic placeholder (unused after Phase D)
│   ├── keyboard.c                 5 keys + GPIO18 long-press state machine
│   ├── shtc3.c                    SHTC3 I²C driver
│   ├── st7306.c                   ST7306 LCD driver + pixel packing
│   ├── hzk16.c                    HZK16 GB2312 font renderer
│   ├── lunar.c                    Lunar calendar algorithm
│   ├── weather.c / caldav.c       HTTP clients (Open-Meteo, Feishu CalDAV)
│   ├── wifi_manager.c             WiFi AP/STA + SNTP
│   └── battery.c                  ADC battery reading
├── components/
│   ├── xiaozhi_core/              XiaoZhi C++ component (OTA/WS/Opus/pipeline)
│   └── codepilot_core/            CodePilot protocol + state + WS server
├── spiffs/HZK16                   GB2312 font file (6763 chars)
├── partitions.csv                 16 MB flash layout
└── sdkconfig.defaults             USB-Serial/JTAG console + HTTPD WS + PSRAM
```

## Architecture

```
                    ┌─────────────────┐
                    │   MENU (top)    │
                    └────────┬────────┘
                             │ ENTER (cursor)
                             ▼
       ┌──────────┬──────────┬──────────┬──────────┬──────────┐
       ▼          ▼          ▼          ▼          ▼          ▼
   Calendar   XiaoZhi    CodePilot    Snake      Tetris
       │          │          │          │          │
       └──────────┴──────────┴──────────┴──────────┘
                             │ BACK (Key4 short)
                             ▼
                    ┌─────────────────┐
                    │   MENU (back)   │
                    └─────────────────┘
```

The `app_manager` dispatcher task is the only thread that calls each app's `on_key` / `on_tick_1s` hooks. Apps spawn their own worker tasks for slow operations (HTTP fetches, game loops, WS receive). Cooperative shutdown via `stop_flag` + semaphore ensures clean resource release on every switch (no `vTaskDelete` strong-arming).

## CodePilot PC bridge

The CodePilot app receives status updates from a PC-side bridge that monitors Claude Code (and optionally Kimi) processes.

```bash
# Clone the bridge (or use your own; wire format is NDJSON over WS)
git clone https://github.com/art-jin/esp32_codepilot
cd esp32_codepilot/bridge
npm install

# Connect to the ESP32 (find IP in boot log or router admin)
node bridge.js --host 192.168.1.87
```

Wire format: one JSON object per WebSocket text frame, `session.update` messages with fields `provider`, `status`, `current_task`, `active`, `quota_used`, `quota_total`.

## License

Apache 2.0. See [LICENSE](LICENSE).

## Acknowledgements

- [Waveshare](https://www.waveshare.com/) for the ESP32-S3-RLCD-4.2 board and reference examples
- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) by 78 for the XiaoZhi voice assistant protocol
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v6.0.1 by Espressif
