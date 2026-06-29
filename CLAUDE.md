# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP-IDF project for the **Waveshare ESP32-S3-RLCD-4.2** development board. Two independent apps — Chinese monthly calendar and XiaoZhi AI voice assistant — switchable via KEY button (GPIO 18). Real-time clock, real weather, Feishu calendar events, WiFi/battery status. Based on ESP-IDF v6.0.1.

### Key Features
- **Two-app architecture**: Calendar and XiaoZhi run as separate apps, switched by KEY button (GPIO 18) with task lifecycle management
- Calendar app: left-right split layout (132px info panel + 268px calendar grid), solar+lunar dates, today highlight, event day marking (dashed rect), rest day marking (striped background), **3-key day navigation** (GPIO1=Prev, GPIO3=Next, GPIO17=Enter) with selection cursor and cross-month support
- Real weather from Open-Meteo API (temperature, humidity, description)
- Real calendar events from Feishu via CalDAV protocol (HTTPS), rendered in 14×14 scaled font
- Bottom status bar: clock (1s refresh), WiFi signal bars, battery icon + %
- WiFi AP config mode (first boot) + STA auto-connect + SNTP time sync
- **XiaoZhi AI voice assistant**: Full-screen app with BOOT button (GPIO 0) voice trigger, continuous multi-turn conversation, event-driven audio pipeline, Opus codec, ES8311/ES7210 hardware

## Build & Flash Commands

```bash
# Activate ESP-IDF environment (required before any build/flash command)
source ~/.espressif/tools/activate_idf_v6.0.1.sh

# Build
python "$IDF_PATH/tools/idf.py" build

# Flash to board
python "$IDF_PATH/tools/idf.py" -p /dev/cu.usbmodem21201 flash

# Flash a raw binary (e.g., factory or third-party firmware)
esptool --chip esp32s3 -p /dev/cu.usbmodem21201 -b 460800 write_flash 0x0 <firmware.bin>

# Serial monitor
python "$IDF_PATH/tools/idf.py" -p /dev/cu.usbmodem21201 monitor
```

Serial port may change — check `/dev/cu.usbmodem*` if the default doesn't connect.

## Architecture

### Core Application (`main/`)

- **hello_world_main.c** — Entry point. Inits display/font/NVS/WiFi, syncs SNTP, fetches weather, registers XiaoZhi callbacks, delegates to app_manager. Defines shared globals (weather data, CalDAV events).
- **app_manager.c / app_manager.h** — App lifecycle manager. GPIO 18 KEY button ISR (300ms debounce) triggers app switch via EventGroup. Deletes/recreates tasks on each switch (calendar `update_task` ↔ XiaoZhi `xiaozhi_run` task). First switch initializes audio hardware via `xiaozhi_init()`. Keyboard integration: `update_task` uses `keyboard_wait()` (EventGroup-driven, ~0ms response) instead of polling.
- **xiaozhi_display.c** — Callback router. Checks `app_manager_current_app()` and forwards XiaoZhi state/text to the active app's display.
- **xiaozhi_app_display.c / xiaozhi_app_display.h** — Full-screen XiaoZhi UI. Top status bar (28px), center "小智"+state text, chat text area (y=210~266, 14px auto-wrap), bottom hints. Reuses `calendar_draw_wifi_bars()` and `calendar_draw_battery_icon()`.
- **st7306.c / st7306.h** — ST7306 LCD driver + graphics primitives (hline, vline, rect, filled_rect, text, 7×14 scaled ASCII, custom 10×20 digit bitmap font). SPI transport via `esp_lcd_panel_io_spi`.
- **hzk16.c / hzk16.h** — HZK16 font file reader (SPIFFS). GB2312 character rendering at 16×16, 14×14, and 12×12. Mixed ASCII/Chinese text. 14×14 and 12×12 are nearest-neighbor scaled from 16×16 bitmaps.
- **lunar.c / lunar.h** — Lunar calendar algorithm (1901-2050). GB2312 pre-encoded tables for month names, day names, weekdays, solar terms.
- **calendar.c / calendar.h** — Calendar rendering: header (solar date in 14px + weekday in native 16×16 + lunar date in 14px), weekday labels (black bg + white text), 6-row grid with today highlight / rest-day stripes / event-day rounded rect markers, left panel (tab-style headers, weather+events in native 16×16 font), status bar (clock+WiFi+battery), bottom "KEY:小智" hint (12px). Exports `calendar_draw_wifi_bars()` and `calendar_draw_battery_icon()` for XiaoZhi app reuse. Holiday/tiaoxiu data tables for 2025-2026. Navigation API: `calendar_select_day(delta)` with partial cell redraw (same-month) or full redraw (cross-month), `calendar_confirm_selection()`, `calendar_clear_selection()`. Selection cursor: 2px-wide nested rectangle outline (white on today's black fill, black on normal cells).
- **keyboard.c / keyboard.h** — 3-key keyboard driver on 2×8PIN header. GPIO1=Prev, GPIO3=Next, GPIO17=Enter. ISR with 300ms debounce, EventGroup-driven `keyboard_wait(timeout)` for instant response + `keyboard_poll()` for non-blocking query.
- **wifi_manager.c / wifi_manager.h** — WiFi AP config mode (HTTP server), STA auto-connect with retry, SNTP time sync, RSSI query.
- **weather.c / weather.h** — Open-Meteo HTTP client. cJSON parsing. WMO weather code → GB2312 Chinese description mapping.
- **battery.c / battery.h** — ADC battery reading (ADC1_CH3/GPIO2, 3x voltage divider, 3.0V-4.12V range).
- **caldav.c / caldav.h** — Feishu CalDAV client. Two-step REPORT (calendar-query + calendar-multiget) over HTTPS with cert bundle. iCal parsing (SUMMARY, DTSTART, DTEND). Heap-allocated buffers (~15KB).
- **utf8_gb2312.c / utf8_gb2312.h** — Runtime UTF-8 to GB2312 conversion. Uses binary search on a 7445-entry static lookup table (`utf8_gb2312_table.h`, auto-generated by `tools/gen_utf8_gb2312.py`).

### XiaoZhi AI Voice Assistant (`components/xiaozhi_core/`)

C++ component providing voice AI via XiaoZhi public server. C-callable API via `xiaozhi_bridge.h` (extern "C").

- **xiaozhi_bridge.cc** — State machine with continuous conversation loop (IDLE→CONNECTING→LISTENING↔SPEAKING), event-driven main loop using `xEventGroupWaitBits` (XZ_EVT_STOP | XZ_EVT_AUDIO_SEND | XZ_EVT_WS_DATA), I2S audio hardware init (ES8311 speaker + ES7210 mic), BOOT button (GPIO 0) trigger. Pipeline and WS client signal bridge event group when data is ready.
- **ota_client.cc** — OTA activation via `https://api.tenclass.net/xiaozhi/ota/`, returns WebSocket URL + token. NVS credential caching.
- **ws_client.cc** — WebSocket client using `esp_websocket_client`. Client/Server Hello handshake, Binary Protocol V1/V2/V3, frame queue to prevent data loss. Signals bridge event group on data received and on connect.
- **opus_codec.cc** — Opus encoder (16kHz mono, 60ms frames, VOIP) and decoder (server sample rate). PSRAM allocation.
- **audio_pipeline.cc** — 3 FreeRTOS tasks + 4 queues: audio_input (Core 0, I2S→resample→encode), opus_codec (Core 1, decode-before-encode priority), audio_output (Core 1, playback→I2S). Signals bridge when audio ready to send. Queue cleanup on reconnect.
- **button_handler.cc** — BOOT button GPIO interrupt with debounce.
- **config.h** — Board pin definitions (I2S, I2C, PA), audio parameters, OTA/server constants.

Audio hardware pins:
| Function | GPIO |
|----------|------|
| I2S MCLK | 16 |
| I2S WS | 45 |
| I2S BCLK | 9 |
| I2S DIN (mic) | 10 |
| I2S DOUT (speaker) | 8 |
| PA enable | 46 |
| I2C SDA (codec ctrl) | 13 |
| I2C SCL (codec ctrl) | 14 |

### ST7306 Pixel Packing (landscape 400x300)

The ST7306 uses a non-standard 2-wide x 4-high block packing. For pixel (x, y):

```
inv_y = 299 - y
byte_x = x / 2,  block_y = inv_y / 4
index = byte_x * 75 + block_y
bit = 7 - ((inv_y % 4) * 2 + (x % 2))
```

Frame buffer update writes columns 0x12-0x2A (25 positions x 3 bytes) x 200 rows = 15,000 bytes via command 0x2C.

### SPI Pin Mapping

| Function | GPIO |
|----------|------|
| CLK      | 11   |
| MOSI     | 12   |
| CS       | 40   |
| DC       | 5    |
| RST      | 41   |

## sdkconfig Notes

- Target: `esp32s3` (ESP32-S3-WROOM-1-N16R8)
- Flash size: 16MB (manually set from default 2MB)
- PSRAM: 8MB octal (CONFIG_SPIRAM_MODE_OCT=y)
- Binary size: ~1.29MB (68% of 4MB app partition free)
- MINIMAL_BUILD enabled (root CMakeLists.txt)

## Reference Materials (`github_reference/`)

Waveshare ESP32-S3-RLCD-4.2 official repo contents. **Not** part of the build — reference only.

### Pre-built Firmware (`03_Firmware/`)

Flash at address `0x0` using `esptool`:

| File | Description |
|------|-------------|
| `01_Factory_V1.bin` | 原厂固件 — 温度/时间显示（4.3MB） |
| `02_XiaoZhi_V2.1.0.bin` | 小智 AI 语音助手 V2.1.0（11.2MB） |

### Example Programs (`02_Example/`)

Each example has both Arduino (`.ino`) and ESP-IDF (`CMakeLists.txt`) versions:

| # | Example | What it demonstrates |
|---|---------|---------------------|
| 01 | WIFI_AP | WiFi 热点模式 |
| 02 | WIFI_STA | WiFi 站点模式 |
| 03 | ADC_Test | ADC 读取（含 BSP 层） |
| 04 | I2C_PCF85063 | I2C RTC 时钟读写 |
| 05 | I2C_SHTC3 | I2C 温湿度传感器 |
| 06 | SD_Card | MicroSD 卡读写 |
| 07 | Audio_Test | 音频编解码（ES8311/ES7210）+ LVGL 显示 |
| 08 | LVGL_V8_Test | LVGL v8 UI 框架 |
| 09 | LVGL_V9_Test | LVGL v9 UI 框架 |
| 10 | FactoryProgram | 原厂出厂程序完整源码（ESP-IDF only） |

### XiaoZhi Source (`02_Example/XiaoZhi/XiaoZhiCode_V2.1.0/`)

小智 AI 语音助手完整源码，基于 ESP-IDF。GitHub: https://github.com/78/xiaozhi-esp32

### Arduino Libraries (`01_Arduino_Libraries/`)

预配置的第三方库：LVGL v8、LVGL v9、SensorLib（传感器/触摸驱动）。

## Adding Chinese Characters

Chinese text uses HZK16 font file (GB2312 encoded, all 6763 chars). Static Chinese strings in code are **pre-encoded as GB2312 byte constants**. Dynamic Chinese text from APIs (weather descriptions, Feishu event titles) is converted at runtime via the UTF-8→GB2312 lookup table (`utf8_gb2312.c`). To regenerate the table: `python3 tools/gen_utf8_gb2312.py > main/utf8_gb2312_table.h`

For custom bitmap glyphs not in HZK16, generate 16x16 bitmaps using Python Pillow:

```python
from PIL import Image, ImageDraw, ImageFont
img = Image.new('1', (16, 16), 0)
draw = ImageDraw.Draw(img)
font = ImageFont.truetype('/System/Library/Fonts/STHeiti Light.ttc', 15)
draw.text((0, 0), '字', font=font, fill=1)
# Export as 2-bytes-per-row C array (32 bytes total)
```

## Display Layouts (400×300)

### Calendar App (default)

```
┌─────────────────────────────────┬──────────────────────────┐
│ █天气█                           │ 2026年5月11日 星期一      │
│ 北京 26度 晴                      │            农历三月廿五    │
│ 湿度 14%                         │█日█一█二█三█四█五█六█│
│─────────────────────────────────│  1  2  3  4  5  6  7  │
│ █代办事宜█                       │  8 ⎾11⏋ 12 13 14  │
│ ● 09:30                         │ 15 16 17 18 19 20 21  │
│   开发板                         │ 22 23 24 25 26 27 28  │
│ - - - - - - - - -               │ 29 30 31              │
│ 05/15 会议                       │                        │
│   周报                           │                        │
│                                 │                        │
│ ── KEY:小智 ──                   │ 14:35:22 ▮▮▮ 🔋85%    │
└─────────────────────────────────┴──────────────────────────┘
  ← 132px (info panel) →← 268px (calendar grid) →
```

### XiaoZhi App (GPIO 18 KEY switches to this)

```
┌──────────────────────────────────────────┐
│ WiFi▮▮   14:35:22           🔋85%       │ 顶部状态栏 (28px)
│──────────────────────────────────────────│
│                                          │
│              [AI 图标]                    │ 中心区域
│              小 智                        │
│                                          │
│         "聆听中..." / "待机"              │ 状态文字
│                                          │
│──────────────────────────────────────────│
│ "今天北京晴朗，温度26度，适合户外活动。"   │ 聊天文字区
│ "你有什么其他问题吗？"                     │ (14px, 多行自动换行)
│──────────────────────────────────────────│
│ GP18:切换日历     BOOT:开始对话            │ 底部提示栏
└──────────────────────────────────────────┘
```

### Calendar Rendering Details

- Header: 14px scaled font for solar/lunar date, native 16×16 for weekday name (e.g. "星期一")
- Weekday row: black background + white text
- Day numbers: custom 10×20 bitmap font (pixel-perfect, no scaling artifacts)
- Today: black filled rect + white number
- Rest days (weekends/holidays): horizontal striped background
- Event days: rounded rectangle border (r=3 Bresenham arc) around the day number
- Left panel: tab-style black headers ("天气" / "代办事宜"), content in native 16×16 font
- Events: 2-line layout per event (time/title), dashed separator between today and upcoming
- Bottom hint (left panel): "KEY:小智" in 12px font
- Status bar: clock (8×16), WiFi signal bars, battery icon + percentage

## App Switching Architecture

```
┌─────────────┐  GPIO 18 KEY press  ┌─────────────┐
│  Calendar   │ ◄──────────────────► │   XiaoZhi   │
│ (update_task)│                     │(xiaozhi_task)│
│  No audio   │                     │  + Audio     │
└─────────────┘                      └─────────────┘
       ▲                                   ▲
       └──── app_manager_task ─────────────┘
              (GPIO 18 ISR + EventGroup + switch logic)
```

- Only one app runs at a time (tasks deleted/recreated on switch)
- `app_manager_task` always runs, monitors GPIO 18 events
- Shared frame buffer and SPI display — full screen clear + redraw on switch
- Calendar → XiaoZhi: delete `update_task` → first-time `xiaozhi_init()` or `xiaozhi_prepare_reconnect()` → create xiaozhi task
- XiaoZhi → Calendar: `xiaozhi_stop_listening()` → delete xiaozhi task → `xiaozhi_disable_audio()` → redraw calendar → create `update_task`

### Keyboard Navigation (Calendar mode only)

3-key keyboard connected via 2×8PIN header on the back. EventGroup-driven for ~0ms response.

| GPIO | Key | Function |
|------|-----|----------|
| GPIO1 | Prev | Move selection cursor to previous day |
| GPIO3 | Next | Move selection cursor to next day |
| GPIO17 | Enter | Confirm selection (logs selected date) |

- Same-month navigation: partial redraw (2 cells + header only, no screen flash)
- Cross-month navigation: full calendar redraw with header lunar date update
- Selection cursor: 2px nested rect (white on today, black on other days)
- `calendar_select_day()` uses `mktime()` for date normalization (handles month/year boundaries)
- Selection cleared on app switch or day change

## Detailed Documentation

- `DEVELOPMENT.md` — Full development walkthrough (init sequence, pixel format, font generation)
- `docs/esp32_rlcd_calendar_prd.md` — Product requirements document with progress tracking
- `docs/dev_esp32_rlcd.md` — Kindle vertical screen porting reference
