idf_component_register(SRCS "app.cpp" "board.cpp" "network.cpp" "player.cpp" "core.cpp" "epaper_driver_bsp.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_adc esp_wifi esp_netif esp_event esp_http_client esp_http_server nvs_flash json esp_timer esp_system esp_hw_support mbedtls
    EMBED_TXTFILES "web/index.html")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-error=misleading-indentation)
