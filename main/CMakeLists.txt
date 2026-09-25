idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES axp2101 audio_hal beta_clanker beta_network beta_reader beta_transcription epaper_panel esp_driver_gpio esp_driver_i2c esp_timer nvs_flash
    EMBED_FILES "clanker_pocket_logo.rle"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
