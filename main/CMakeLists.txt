idf_component_register(SRCS "hello_world_main.c" "st7306.c" "hzk16.c" "lunar.c"
                            "calendar.c" "wifi_manager.c" "battery.c" "weather.c"
                            "caldav.c" "utf8_gb2312.c" "xiaozhi_display.c"
                            "app_manager.c" "app_registry.c"
                            "menu_app.c" "calendar_app.c" "xiaozhi_app.c"
                            "codepilot_app.c"
                            "shtc3.c"
                            "snake_app.c"
                            "tetris_app.c"
                            "placeholder_app.c"
                            "xiaozhi_app_display.c"
                            "keyboard.c"
                       PRIV_REQUIRES spi_flash esp_lcd driver spiffs
                                    esp_wifi nvs_flash esp_netif esp_http_server
                                    esp_http_client esp_adc esp-tls
                                    esp_timer
                                    xiaozhi_core codepilot_core
                       INCLUDE_DIRS "")
