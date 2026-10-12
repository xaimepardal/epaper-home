# CA propia del broker (p. ej. la del NAS): main/certs/mqtt_ca.crt. Si existe se
# incrusta en el firmware; si no, mqtts:// valida con el bundle de CAs públicas.
set(ca_propia "${CMAKE_CURRENT_SOURCE_DIR}/certs/mqtt_ca.crt")
set(embed "")
if(EXISTS "${ca_propia}")
    set(embed "certs/mqtt_ca.crt")
endif()

idf_component_register(SRCS "epaper_mqtt.cpp" "wifi.cpp" "battery.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_driver_gpio esp_timer esp_rom
                                     mbedtls esp-tls esp_app_format esp_adc
                       EMBED_TXTFILES ${embed})

if(EXISTS "${ca_propia}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MQTT_CUSTOM_CA=1)
    message(STATUS "MQTT: CA propia ${ca_propia}")
endif()
# reconfigurar si se añade o quita la CA
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/certs")
