idf_component_register(
    SRCS "beta_network.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_http_client esp_http_server nvs_flash mbedtls
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
