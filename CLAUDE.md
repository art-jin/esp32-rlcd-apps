# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Multi-app firmware for the **Waveshare ESP32-S3-RLCD-4.2** (4.2" reflective LCD). A MENU launcher hosts 5 apps: **Calendar**, **XiaoZhi** (voice AI), **CodePilot** (PC bridge dashboard), **Snake**, and **Tetris**. ESP-IDF v6.0.1, target `esp32s3`, 16 MB flash, 8 MB octal PSRAM.

The KinCal factory build enables only **Calendar** (the other four are gated by `CONFIG_KINCAL_APP_*` Kconfig flags, default `n`). Single-App factory firmware boots directly into Calendar, skipping the MENU screen entirely — this is intentional so the original 3-key Waveshare board (only GPIO18 usable for App UI) isn't stranded on a screen that requires `KEY_ENTER` to leave.

See `README.md` (English) / `README_zh.md` (中文) for the user-facing app list, GPIO map, and build/flash quick-start. Verified vs pending hardware-verification status per app is tracked in the README table.

## Build & Flash

```bash
# ESP-IDF env must be active before any idf.py / esptool command
source ~/.espressif/tools/activate_idf_v6.0.1.sh

idf.py build                                                     # build
idf.py -p /dev/cu.usbmodem21201 flash                            # flash
idf.py -p /dev/cu.usbmodem21201 monitor                          # serial monitor (Ctrl-] to exit)

# Flash a raw binary (factory or third-party firmware) at offset 0x0
esptool --chip esp32s3 -p /dev/cu.usbmodem21201 -b 460800 write_flash 0x0 <firmware.bin>
```

The serial port may vary — check `/dev/cu.usbmodem*` if the default doesn't connect. Console output goes via USB-Serial/JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), so the same USB port used for flashing also carries logs.

`pytest_hello_world.py` is the ESP-IDF boilerplate; it expects `Hello world!` which this app does not print — it is not a meaningful test here.

## App Framework (the central abstraction)

`main/app_framework.h` defines the `app_t` interface and `app_id_t` enum. Every app implements some subset of four hooks:

| Hook | Contract |
|------|----------|
| `on_enter` | Allocate resources, draw first screen, spawn worker task. |
| `on_exit` | **Cooperative** — set `stop_flag`, wait for worker self-delete via semaphore (≤2s), force-kill only as last-resort fallback. Never call `vTaskDelete(worker)` directly. |
| `on_key` | Route key events. Called from the single dispatcher task → single-threaded, no locking needed between hooks. |
| `on_tick_1s` | Optional 1 Hz heartbeat. `NULL` = no tick. |

`main/app_registry.c` statically registers all 6 apps. To add a new app: pick the next `APP_ID_*`, add it to the enum + registry, implement the hooks, and add its display name (GB2312 pre-encoded bytes for Chinese).

`main/app_manager.c` runs the single **dispatcher task**. It blocks on `keyboard_wait()` for up to 1 s; a key event routes to `current_app->on_key`, a timeout routes to `on_tick_1s`. This is the only thread that calls `on_key` / `on_tick_1s`, so hooks can mutate app state without locks. Worker tasks that render must wrap multi-call draw sequences in `app_manager_display_lock()` / `app_manager_display_unlock()` (recursive mutex) to avoid tearing against the dispatcher.

### Cooperative lifecycle pattern

All apps with worker tasks follow the same shape (see `calendar_app.c`, `xiaozhi_app.c`, `codepilot_app.c`, `snake_app.c`, `tetris_app.c`):

```c
static volatile bool s_stop_flag;
static SemaphoreHandle_t s_exit_sem;   // binary, created once
static TaskHandle_t s_worker;

static void worker(void *arg) {
    while (!s_stop_flag) { ... vTaskDelay(...); }
    xSemaphoreGive(s_exit_sem);
    s_worker = NULL;
    vTaskDelete(NULL);
}

void app_on_enter(void) {
    s_stop_flag = false;
    if (!s_exit_sem) s_exit_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_exit_sem, 0);                 // drain
    ...draw initial screen under display_lock...
    xTaskCreate(worker, ...);
}

void app_on_exit(void) {
    s_stop_flag = true;
    if (s_worker && xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(2000)) != pdPASS) {
        ESP_LOGE(TAG, "Worker did not exit in 2s, force killing");
        vTaskDelete(s_worker);                      // last resort only
        s_worker = NULL;
    }
}
```

`app_manager_switch(target)` calls current app's `on_exit`, then target's `on_enter`. Apps return to MENU via `app_manager_switch(APP_ID_MENU)` from their `KEY_BACK` handler.

### Display sharing

There is one frame buffer and one SPI display. Switching apps calls `st7306_clear()` + full redraw in the new app's `on_enter`. The display mutex is recursive (same task can lock multiple times).

## Keyboard (single-key + 4-key dual modes)

`main/keyboard.c` reads a hardware flag from NVS (`wifi_creds` namespace, key `has_back_key`) at init time and adapts its routing accordingly. The flag is set via the AP captive portal's "我有 4 键硬件（带 BACK 按钮）" checkbox.

### Hardware variants

| Variant | Physical buttons | GPIO |
|---|---|---|
| **Factory single-key** (default, `has_back_key=false`) | BOOT / PWR / KEY only — KEY (GPIO18) is the **sole** button usable for App UI | 0, 18 |
| **4-key extension** (`has_back_key=true`) | + PREV / NEXT / ENTER / BACK external keyboard | 1, 3, 17, 43 |

GPIO43 (BACK) is skipped on single-key hardware — the pin stays floating/unconfigured.

### Event types

| GPIO | Event | Behavior |
|---|---|---|
| 1 | `KEY_PREV` | NEGEDGE ISR, 300 ms global debounce (4-key hw only) |
| 3 | `KEY_NEXT` | " |
| 17 | `KEY_ENTER` | " |
| 43 | `KEY_BACK` | " (4-key hw only) |
| 18 short | `KEY_USER` | ANYEDGE ISR + 50 ms `esp_timer` poll; **single-key hw defers emission by 250 ms** to detect double-click |
| 18 short×2 | `KEY_DOUBLE_CLICK` | Single-key hw only: two short presses within 250 ms |
| 18 hold | `KEY_LONG_START` / `KEY_LONG_END` | 500 ms threshold |

### Dispatcher routing (`main/app_manager.c`)

The dispatcher task does **not** blindly alias `KEY_USER → KEY_BACK` anymore. Routing rules:

- **4-key hardware**: every event delivered as-is. Apps decide.
- **Single-key hardware, MENU app**: `KEY_USER` and `KEY_DOUBLE_CLICK` delivered as-is (MENU's state machine needs them for next/confirm).
- **Single-key hardware, non-MENU apps**: `KEY_USER` is aliased to `KEY_BACK` so existing apps that only handle `KEY_BACK` (Snake / Tetris / XiaoZhi / CodePilot) keep working without modification. `KEY_DOUBLE_CLICK` and `KEY_LONG_*` are delivered as-is.

### Single-key navigation matrix

| Context | Short press GPIO18 | Double-click GPIO18 | Long press GPIO18 |
|---|---|---|---|
| MENU (multi-App) | Cursor next item (wrap) | Enter selected App | No-op |
| Calendar (single-App firmware) | Clear selection / snap to today | (deferred → KEY_USER) | No-op (no menu to return to) |
| Calendar (multi-App firmware) | Clear selection / snap to today | (deferred → KEY_USER) | Return to MENU |
| Other Apps (Snake/Tetris/XiaoZhi/CodePilot) | Aliased to BACK (exit to menu) | App-specific | App-specific (CodePilot uses for STT) |

### Single-App factory firmware

When `app_registry_count_enabled() == 1` (KinCal factory build — only Calendar), the dispatcher **skips MENU entirely** at boot and enters Calendar directly. This avoids stranding single-key users on a MENU screen that requires `KEY_ENTER` to leave. Multi-App builds still boot into MENU.

See `docs/single_key_navigation.md` for the full design rationale and interaction table.

## Per-app notes

- **MENU** (`menu_app.c`) — Vertical launcher. 4-key hw: PREV/NEXT moves cursor (wraps), ENTER switches app. Single-key hw: short-press cycles cursor, double-click switches. Default cursor = Calendar. Bottom hint adapts to hardware (`"KEY:选择 双击:进入"` vs `"PREV/NEXT:选择 OK:进入"`). Reuses `xiaozhi_app_draw_status_bar()`. Skipped entirely at boot when only one App is enabled (factory single-App firmware boots straight into Calendar).
- **Calendar** (`calendar_app.c` + rendering lib `calendar.c`) — 168 px info panel + 232 px month grid (CELL_W=33, CELL_H=38). Worker does 1 Hz status-bar refresh, KinCal JSON fetch on dynamic interval (60 s Pro/Biz, 300 s Free — server-controlled), 60-s SHTC3 indoor temp/humidity. PREV/NEXT/ENTER = day navigation, BACK = clear selection / return to menu (no-op in single-App firmware). Long-press GPIO18 = return to menu (single-key hw, multi-App only). Holiday/tiaoxiu tables for 2025–2026. Selection cursor is a 2 px nested rectangle (white on today, black elsewhere). Exports `calendar_draw_wifi_bars()` / `calendar_draw_battery_icon()` for reuse by other apps.
- **XiaoZhi** (`xiaozhi_app.c` + `components/xiaozhi_core/`) — C++ component providing voice AI over WebSocket to XiaoZhi public server. `xiaozhi_bridge.h` is the `extern "C"` API. Continuous conversation loop (IDLE→CONNECTING→LISTENING↔SPEAKING), event-driven main loop on `xEventGroupWaitBits`, Opus codec, 3-task audio pipeline (`audio_pipeline.cc`), ES8311 speaker + ES7210 mic. `xiaozhi_init()` is **idempotent** — first call does full audio init at boot; subsequent calls return `ESP_OK` without re-initializing hardware. BACK = return to menu, ENTER = trigger conversation (BOOT button GPIO0 also works).
- **CodePilot** (`codepilot_app.c` + `components/codepilot_core/`) — Receives Claude/Kimi agent status from a PC bridge over WebSocket on port 7897 (`ws_server.c`). 3 cycleable views: SPLIT (Claude + Kimi side by side), DETAIL (Claude large), NOTIFY. Long-press GPIO18 triggers XiaoZhi STT with speaker muted (`xiaozhi_set_speaker_mute(true)` + `xiaozhi_start_listening()`). Spawns both a WS-drain worker and a XiaoZhi STT task. PC bridge: <https://github.com/art-jin/esp32_codepilot>, wire format is NDJSON `session.update` frames.
- **Snake** (`snake_app.c`) — 25×15 grid of 16 px cells. PREV/NEXT = relative-turn (Nokia style), ENTER = pause/resume, BACK = menu. Speeds up every 5 food eaten.
- **Tetris** (`tetris_app.c`) — 10×20 grid of 12 px cells. PREV/NEXT = move, ENTER = rotate CW, BACK short = menu, **BACK long** (KEY_LONG_START) = hard drop. 7 pieces × 4 rotations, level up every 10 lines.

## Components

- `components/xiaozhi_core/` — C++ voice AI. `src/xiaozhi_bridge.cc` is the state machine + `extern "C"` API; `ota_client.cc` activates via `api.tenclass.net` and returns WS URL + token; `ws_client.cc` is the WebSocket client with V1/V2/V3 binary protocol; `opus_codec.cc` is encoder (16 kHz mono, 60 ms VOIP frames) + decoder; `audio_pipeline.cc` runs 3 FreeRTOS tasks on cores 0/1/1; `button_handler.cc` is BOOT GPIO0; `config.h` has board pin definitions.
- `components/codepilot_core/` — Pure C. `ws_server.c` wraps `esp_http_server` WebSocket on port 7897 with a receive queue; `protocol.c` parses NDJSON `session.update` via cJSON; `state_manager.c` holds `global_state_t` with mutex-protected reads (`state_manager_lock()`/`unlock()` around snapshot reads during render).

## Display driver specifics

ST7306 LCD, landscape 400×300, non-standard **2-wide × 4-high block packing**. For pixel (x, y):

```
inv_y = 299 - y
byte_x = x / 2,  block_y = inv_y / 4
index = byte_x * 75 + block_y
bit   = 7 - ((inv_y % 4) * 2 + (x % 2))
```

Frame buffer update writes columns 0x12–0x2A (25 positions × 3 bytes) × 200 rows = 15 000 bytes via command 0x2C. Driver in `main/st7306.c` exposes hline / vline / rect / filled_rect / text (7×14 scaled ASCII) / 10×20 bitmap digit font, plus `st7306_update_display()` which must be called after any draw sequence to flush to the panel.

## Fonts and Chinese text

`main/hzk16.c` reads `spiffs/HZK16` (GB2312, all 6763 chars) and renders at native 16×16, plus nearest-neighbor-scaled 14×14 and 12×12. Mixed ASCII/Chinese is supported.

- **Static Chinese strings in code** are pre-encoded as GB2312 byte arrays (see `app_registry.c` for the menu names, `menu_app.c`/`codepilot_app.c` for hints). Use a Python console or similar to encode: `print([hex(b) for b in '选择'.encode('gb2312')])`.
- **Dynamic Chinese from APIs** (weather descriptions, Feishu event titles, CodePilot STT text) is converted at runtime via `main/utf8_gb2312.c` — binary search over a 7445-entry static table. Regenerate with `python3 tools/gen_utf8_gb2312.py > main/utf8_gb2312_table.h`.
- **Custom glyphs not in HZK16**: generate a 16×16 bitmap with Pillow (`Image.new('1', (16,16), 0)` + `.truetype(...)` at size 15) and export as 2-bytes-per-row C array.

## GPIO pin map (consolidated)

| Function | GPIO |
|----------|------|
| SPI CLK / MOSI / CS / DC / RST | 11 / 12 / 40 / 5 / 41 |
| I2S MCLK / BCLK / WS / DIN (mic) / DOUT (speaker) | 16 / 9 / 45 / 10 / 8 |
| Speaker amp enable | 46 |
| I²C SDA / SCL (codecs + SHTC3 @ 0x70) | 13 / 14 |
| Battery ADC (3× divider) | 2 |
| BOOT (XiaoZhi voice trigger) | 0 |
| PREV / NEXT / ENTER / BACK (external keyboard, 4-key hw only) | 1 / 3 / 17 / 43 |
| KEY / USER / LONG (single-key hw: short + long; 4-key hw: USER + long) | 18 |

> **Hardware variants**: factory single-key hardware has only BOOT (GPIO0) + PWR + KEY (GPIO18) — no PREV/NEXT/ENTER/BACK. The 4-key extension adds GPIO1/3/17/43 via the 2×8 header. GPIO43 was originally UART0 TX; USB-Serial/JTAG console frees it. Single-key hw routes GPIO18 through double-click detection (250 ms window) and the dispatcher decides per-app whether to alias `KEY_USER → KEY_BACK` (see [Keyboard](#keyboard-single-key--4-key-dual-modes) section above).

## sdkconfig highlights (`sdkconfig.defaults`)

- 16 MB flash, custom partition table (`partitions.csv`): factory app at 0x10000 (4 MB), SPIFFS at 0x410000 (~3.9 MB for HZK16).
- 8 MB octal PSRAM, 512-byte internal reservation threshold, 64 KB internal reserve.
- C++ exceptions enabled (required by XiaoZhi).
- Code-size optimization + newlib nano format.
- mbedtls dynamic buffer + external mem alloc (saves RAM on TLS handshakes).
- `CONFIG_HTTPD_WS_SUPPORT=y` (CodePilot WebSocket server).
- Watchdog timeout 10 s.

## Common dev tasks

- **Add a Chinese string**: encode it to GB2312 offline, drop the bytes into a `static const uint8_t[]` with a trailing `0`, render with `hzk16_draw_gb_text(x, y, bytes, color)`. Width via `hzk16_text_width(bytes)`.
- **Render a multi-call draw sequence from a worker**: wrap in `app_manager_display_lock()` / `app_manager_display_unlock()` and call `st7306_update_display()` once at the end.
- **Add a new app**: extend `app_id_t`, register in `app_registry.c`, add to `s_menu_items[]` in `menu_app.c`, add the source file to `main/CMakeLists.txt`. Gate it with `CONFIG_KINCAL_APP_*` so it stays off in the factory single-App build.
- **Test single-key navigation without 4-key hw**: temporarily add `s_has_back_key = false;` after the NVS read in `keyboard_init()` to force the single-key code path. Boot log should show `+ double-click (250ms window)` suffix. Revert before committing.
- **Modify XiaoZhi audio**: pin definitions are in `components/xiaozhi_core/src/config.h`. Codec parameters (sample rate, frame size) live in `opus_codec.cc`.

## Reference materials

`github_reference/` holds the Waveshare ESP32-S3-RLCD-4.2 official repo contents — **not** part of the build. Includes pre-built factory and XiaoZhi firmware (`03_Firmware/`), example programs (`02_Example/`), Arduino libraries, and the upstream XiaoZhi source (`02_Example/XiaoZhi/XiaoZhiCode_V2.1.0/`, GitHub: <https://github.com/78/xiaozhi-esp32>).

## Further reading

- `DEVELOPMENT.md` — Full development walkthrough (init sequence, pixel format, font generation).
- `docs/single_key_navigation.md` — Single-key navigation design (double-click detection, MENU skip, interaction matrix).
- `docs/test_cases.md` — Test case matrix (TC-1 startup routing, TC-2 double-click state machine, TC-3/4 single-key UI, TC-5 4-key regression, TC-6 KinCal, TC-7 UI, TC-8 fault modes).
- `docs/esp32_rlcd_calendar_prd.md` — Product requirements document with progress tracking.
- `docs/dev_esp32_rlcd.md` — Kindle vertical screen porting reference.
- `README.md` / `README_zh.md` — User-facing app overview and quick-start.
