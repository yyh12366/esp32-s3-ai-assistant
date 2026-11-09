cmake_minimum_required(VERSION 3.16)
project(astr2esp32_ui_preview C)

set(FW "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)

add_subdirectory("${FW}/managed_components/lvgl__lvgl" lvgl)
find_package(SDL2 REQUIRED CONFIG)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

set(M "${FW}/main")
add_executable(ui_preview
    preview.c
    preview_cli.c
    preview_media.c
    preview_http.c
    preview_audio.c
    "${M}/av_ui.c"
    "${M}/av_url.c"
    "${M}/kk_core.c"
    "${M}/kk_anim.c"
    "${M}/kk_fb.c"
    "${M}/kk_render.c"
    "${M}/kk_render_home.c"
    "${M}/kk_render_pages.c"
    "${M}/kk_render_modal.c"
    "${M}/kk_render_settings.c"
    "${M}/kk_render_targets.c"
    "${M}/kk_render_texts.c"
    "${M}/kk_icons.c"
    "${M}/kk_font.c"
    "${FW}/assets/fonts/av_font_cjk_16.c"
    "${FW}/assets/images/av_character_idle.c"
    "${FW}/assets/images/av_character_blink.c"
    "${FW}/assets/images/av_character_listening.c"
    "${FW}/assets/images/av_character_thinking.c"
    "${FW}/assets/images/av_character_speaking.c"
)
target_include_directories(ui_preview BEFORE PRIVATE include)
target_include_directories(ui_preview PRIVATE "${M}")
target_compile_definitions(ui_preview PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_link_libraries(ui_preview PRIVATE lvgl SDL2::SDL2 CURL::libcurl Threads::Threads)
