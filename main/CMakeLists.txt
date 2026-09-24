idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES axp2101 audio_hal beta_network epaper_panel esp_driver_gpio esp_driver_i2c esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
