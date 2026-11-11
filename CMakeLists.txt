cmake_minimum_required(VERSION 3.16)

# Vendored components (esp_codec_dev)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Work around long command lines on Windows
set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_nes)

# Headless build for a bare ESP32-S3 dev board: no ST7789/ES8388/XL9555.
# Build with: idf.py -B build_headless -DNES_HEADLESS=ON build
option(NES_HEADLESS "Build without local display/audio (TV streaming only)" OFF)
if(NES_HEADLESS)
    add_compile_definitions(NES_HEADLESS=1)
endif()
