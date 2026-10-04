idf_component_register(
    SRCS "beta_reader.cpp" "reader_font.cpp"
    INCLUDE_DIRS "."
    REQUIRES fatfs sdmmc esp_full_miniz
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
