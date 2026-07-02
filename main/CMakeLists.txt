# Core sources always compiled: MENU + Calendar + display/font/wifi/battery +
# SHTC3 sensor + XiaoZhi shared bits reused by core code (status bar drawing,
# text/state callback router). These are the minimum for the KinCal-only
# factory firmware.
set(SRCS
    hello_world_main.c
    st7306.c
    hzk16.c
    lunar.c
    calendar.c
    wifi_manager.c
    battery.c
    utf8_gb2312.c
    keyboard.c
    shtc3.c
    app_manager.c
    app_registry.c
    menu_app.c
    calendar_app.c
    # XiaoZhi UI helpers reused by menu/calendar and other apps. Compiled in
    # even when CONFIG_KINCAL_APP_XIAOZHI is off — they don't pull in the
    # voice assistant lifecycle, just the shared status-bar drawing and the
    # text/state callback router that hello_world_main.c wires up at boot.
    xiaozhi_app_display.c
    xiaozhi_display.c
)

# KinCal JSON client + bridge (Phase 4 default data source). Compiled in
# unless explicitly using the legacy direct CalDAV/Open-Meteo path.
if(NOT CONFIG_KINCAL_LEGACY_DIRECT_APIS)
    list(APPEND SRCS kincal_client.c kincal_bridge.c)
endif()

# Legacy direct APIs (CalDAV/Open-Meteo). Compiled only when explicitly enabled
# as a fallback. Default off once KinCal integration is the primary data path.
if(CONFIG_KINCAL_LEGACY_DIRECT_APIS)
    list(APPEND SRCS caldav.c weather.c)
endif()

# Optional apps — toggled from Kconfig. Each flag adds the app's source(s)
# and conditionally registers it in app_registry.c / menu_app.c.
# XiaoZhi: only the app lifecycle file is gated. xiaozhi_app_display.c and
# xiaozhi_display.c are always compiled because menu_app.c / hello_world_main.c
# depend on them.
if(CONFIG_KINCAL_APP_XIAOZHI)
    list(APPEND SRCS xiaozhi_app.c)
endif()

if(CONFIG_KINCAL_APP_CODEPILOT)
    list(APPEND SRCS codepilot_app.c)
endif()

if(CONFIG_KINCAL_APP_SNAKE)
    list(APPEND SRCS snake_app.c)
endif()

if(CONFIG_KINCAL_APP_TETRIS)
    list(APPEND SRCS tetris_app.c)
endif()

if(CONFIG_KINCAL_APP_TOWER)
    list(APPEND SRCS tower_defense_app.c)
endif()

# Conditional component requirements.
# xiaozhi_core is always required: SHTC3 reuses its I2C bus, hello_world_main
# calls xiaozhi_init() at boot, and xiaozhi_app_display pulls in xiaozhi_bridge.h.
# esp_http_server is always required: wifi_manager.c captive portal uses it.
# codepilot_core is unconditionally required as a dependency even when the
# CodePilot app is disabled — its component is always in the build tree and
# the always-compiled xiaozhi_app_display.c path doesn't reference it, but
# making it conditional breaks the build due to ESP-IDF component resolution
# ordering. The app-level gating happens via SRCS and app_registry.c.
set(PRIV_REQUIRES
    spi_flash
    esp_lcd
    driver
    spiffs
    esp_wifi
    nvs_flash
    esp_netif
    esp_http_client
    esp_http_server
    esp-tls
    esp_timer
    esp_adc
    xiaozhi_core
    codepilot_core
)

idf_component_register(SRCS ${SRCS}
                       PRIV_REQUIRES ${PRIV_REQUIRES}
                       INCLUDE_DIRS "")
