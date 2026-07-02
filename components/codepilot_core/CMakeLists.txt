idf_component_register(SRCS "protocol.c" "state_manager.c" "ws_server.c" "serial_input.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server espressif__cjson driver)
