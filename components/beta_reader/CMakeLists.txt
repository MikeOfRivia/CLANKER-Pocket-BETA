idf_component_register(
    SRCS "beta_reader.cpp"
    INCLUDE_DIRS "."
    REQUIRES fatfs sdmmc
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
