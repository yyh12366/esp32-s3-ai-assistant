# Component discovery scans this file before the APP_VARIANT cache variable
# reaches subdirectories, so the top-level CMakeLists mirrors it into the env.
set(_app_variant "$ENV{APP_VARIANT}")
if(NOT _app_variant)
    set(_app_variant "kk_oled")
endif()

if(_app_variant STREQUAL "astrbot_voice")
    message(STATUS "main CMake: astrbot_voice")
    set(APP_SOURCES
        "main_astrbot_voice.c"
        "av_boot.c"
        "av_shell.c"
        "av_shell_boot.c"
        "av_settings.c"
        "av_app.c"
        "av_audio.c"
        "av_adpcm.c"
        "av_net.c"
        "av_ui.c"
        "av_wifi.c"
        "av_blufi_security.c"
        "av_store.c"
        "av_session.c"
        "av_proto.c"
        "av_ws.c"
        "av_url.c"
        "av_text.c"
        "av_ring.c"
        "av_ipedit.c"
        "av_anim.c"
        "av_ota.c"
        "av_ota_manifest.c"
    )
    idf_component_register(
        SRCS ${APP_SOURCES}
        INCLUDE_DIRS "."
        REQUIRES app_update bsp bt esp_app_format esp_event esp_http_client esp_netif esp_timer esp_wifi lwip mbedtls nvs_flash
    )
elseif(_app_variant STREQUAL "allinone")
    message(STATUS "main CMake: allinone (home shell: voice + books/music + settings, KK 1-bit monitor)")
    set(APP_SOURCES
        "main_allinone.c"
        "kk_boot.c"
        "av_shell.c"
        "av_shell_boot.c"
        "av_settings.c"
        "kk_app.c"
        "kk_app_input.c"
        "kk_app_telemetry.c"
        "kk_audio.c"
        "kk_core.c"
        "kk_anim.c"
        "kk_fb.c"
        "kk_monitor.c"
        "kk_render_home.c"
        "kk_render.c"
        "kk_render_pages.c"
        "kk_render_modal.c"
        "kk_render_settings.c"
        "kk_render_targets.c"
        "kk_render_texts.c"
        "kk_store.c"
        "kk_store_worker.c"
        "kk_icons.c"
        "kk_font.c"
        "av_app.c"
        "av_audio.c"
        "av_adpcm.c"
        "av_net.c"
        "av_ui.c"
        "av_wifi.c"
        "av_blufi_security.c"
        "av_store.c"
        "av_session.c"
        "av_proto.c"
        "av_ws.c"
        "av_url.c"
        "av_text.c"
        "av_ring.c"
        "av_ipedit.c"
        "av_anim.c"
        "av_media.c"
        "av_media_ui.c"
        "av_media_app.c"
        "av_ota.c"
        "av_ota_manifest.c"
    )
    idf_component_register(
        SRCS ${APP_SOURCES}
        INCLUDE_DIRS "."
        REQUIRES app_update bsp bt esp_app_format esp_event esp_http_client esp_hw_support esp_netif esp_timer esp_wifi heap lwip mbedtls nvs_flash
    )
else()
    message(STATUS "main CMake: kk_oled")
    set(APP_SOURCES
        "main.c"
        "kk_app.c"
        "kk_app_input.c"
        "kk_app_telemetry.c"
        "kk_audio.c"
        "kk_core.c"
        "kk_anim.c"
        "kk_fb.c"
        "kk_monitor.c"
        "kk_render_home.c"
        "kk_render.c"
        "kk_render_pages.c"
        "kk_render_modal.c"
        "kk_render_settings.c"
        "kk_render_targets.c"
        "kk_render_texts.c"
        "kk_store.c"
        "kk_store_worker.c"
        "kk_icons.c"
        "kk_font.c"
    )
    idf_component_register(
        SRCS ${APP_SOURCES}
        INCLUDE_DIRS "."
        REQUIRES bsp esp_hw_support esp_timer heap nvs_flash
    )
endif()

if(_app_variant STREQUAL "astrbot_voice" OR _app_variant STREQUAL "allinone")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    target_sources(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/av_font_cjk_16.c"
        "${CMAKE_CURRENT_LIST_DIR}/../assets/images/av_character_idle.c"
        "${CMAKE_CURRENT_LIST_DIR}/../assets/images/av_character_blink.c"
        "${CMAKE_CURRENT_LIST_DIR}/../assets/images/av_character_listening.c"
        "${CMAKE_CURRENT_LIST_DIR}/../assets/images/av_character_thinking.c"
        "${CMAKE_CURRENT_LIST_DIR}/../assets/images/av_character_speaking.c"
    )
endif()

# 合并固件：监测退出时需要回到首页（kk_app.c 里 KK_HAS_SHELL 才会调用 av_shell_reboot_to）
if(_app_variant STREQUAL "allinone")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KK_HAS_SHELL)
endif()

# 独立语音固件：没有首页、小说、音乐（av_shell.c 里编译掉对应代码）
if(_app_variant STREQUAL "astrbot_voice")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AV_SHELL_VOICE_ONLY)
endif()

if(IDF_TARGET STREQUAL "esp32s3" AND _app_variant STREQUAL "allinone")
    target_sources(${COMPONENT_LIB} PRIVATE "s3_video.c" "s3_video_display.c" "video_protocol.c")
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_driver_usb_serial_jtag)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AV_S3_VIDEO)
endif()
