idf_component_register(
  SRCS "main.cpp" "board.cpp" "console_cmds.cpp" "sampler.cpp" "indicator.cpp" "net.cpp" "web.cpp" "notify.cpp"
       "hal/flash_store.cpp" "hal/nvs_store.cpp" "hal/rs485_uart.cpp"
  INCLUDE_DIRS "."
  EMBED_TXTFILES "www/index.html"
  EMBED_FILES "../../assets/logo_ReefDO.svg" "www/apple-touch-icon.png"
  REQUIRES reefdo_core esp_driver_gpio esp_driver_ledc esp_driver_rmt esp_driver_uart console esp_timer esp_system
           esp_app_format esp_partition nvs_flash led_strip esp_wifi esp_netif esp_event esp_http_server
           esp_http_client esp-tls mbedtls app_update mdns)

# IDF v6 compiles C++ as gnu++26 by default; the core component pins gnu++23 to stay identical to the host build.
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Werror)
