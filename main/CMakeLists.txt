idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES axp2101 epaper_panel esp_driver_gpio esp_driver_i2c
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
