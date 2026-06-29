idf_component_register(SRCS "protocol.c" "state_manager.c" "ws_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server espressif__cjson)
