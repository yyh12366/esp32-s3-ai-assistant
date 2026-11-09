cmake_minimum_required(VERSION 3.16)

set(APP_VARIANT "allinone" CACHE STRING "Firmware application: allinone, kk_oled or astrbot_voice")
set_property(CACHE APP_VARIANT PROPERTY STRINGS kk_oled astrbot_voice allinone)
if(NOT APP_VARIANT STREQUAL "kk_oled" AND NOT APP_VARIANT STREQUAL "astrbot_voice"
   AND NOT APP_VARIANT STREQUAL "allinone")
    message(FATAL_ERROR "APP_VARIANT must be kk_oled, astrbot_voice or allinone")
endif()

# Component scanning runs before this cache variable is visible to subdirectories,
# so mirror it into the environment for main/CMakeLists.txt to read.
set(ENV{APP_VARIANT} "${APP_VARIANT}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(IDF_TARGET STREQUAL "esp32s3")
    idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.esp32s3)
endif()

project(FoloToy-AI-Passport)
