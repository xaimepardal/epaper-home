idf_component_register(SRCS "epaper_mqtt.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_driver_gpio esp_timer esp_rom)
