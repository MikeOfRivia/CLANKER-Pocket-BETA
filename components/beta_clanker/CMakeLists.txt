idf_component_register(
    SRCS "beta_clanker.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json nvs_flash mbedtls
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
