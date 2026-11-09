idf_component_register(
    SRCS "es8311_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_codec_dev
    PRIV_REQUIRES esp_timer
)
