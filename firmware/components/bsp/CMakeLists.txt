if(IDF_TARGET STREQUAL "esp32s3")
    set(BSP_BUTTON_SOURCE "src/bsp_button_usb.c")
    set(BSP_USB_REQUIRES esp_driver_usb_serial_jtag)
else()
    set(BSP_BUTTON_SOURCE "src/bsp_button.c")
endif()

idf_component_register(
    SRCS "src/bsp_i2c.c"
         "src/bsp_display.c"
         "src/bsp_display_mono.c"
         "src/bsp_display_mono_math.c"
         "src/bsp_display_lvgl.c"
         "src/bsp_display_rounding.c"
         ${BSP_BUTTON_SOURCE}
         "src/bsp_audio.c"
         "src/bsp_es8311_sleep_check.c"
         "src/bsp_battery.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_driver_i2s esp_lcd esp_lvgl_port button esp_adc esp_codec_dev
    PRIV_REQUIRES esp_timer ${BSP_USB_REQUIRES}
)
