idf_component_register(
    SRCS "src/xiaozhi_bridge.cc"
         "src/ota_client.cc"
         "src/ws_client.cc"
         "src/opus_codec.cc"
         "src/audio_pipeline.cc"
         "src/button_handler.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_codec_dev
                  esp_http_client nvs_flash esp-tls esp_event
                  spi_flash esp_websocket_client
)
