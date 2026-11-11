# ESP32-NES main component

set(APP_SOURCES
    "app_main.cpp"
    "board/board_init.cpp"
    "board/font_cjk.cpp"
    "display/panel_display.cpp"
    "display/nes_display.cpp"
    "display/menu_ui.cpp"
    "audio/audio_out.cpp"
    "audio/nes_audio.cpp"
    "input/input.cpp"
    "input/ble_gamepad.cpp"
    "input/ble_raw_hid.cpp"
    "input/hid_report.cpp"
    "input/esp_hid_gap.c"
    "rom/rom_store.cpp"
    "rom/web_portal.cpp"
    "nes/nes_stream.cpp"
)

# InfoNES core. mapper/*.cpp are textually included by InfoNES_Mapper.cpp,
# so only compile the top-level .cpp files.
file(GLOB NES_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/nes/*.cpp")
# nes_stream.cpp is an app module, listed explicitly above.
list(REMOVE_ITEM NES_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/nes/nes_stream.cpp")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set_source_files_properties(${NES_CORE_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-Wno-error=type-limits;-Wno-error=array-bounds;-Wno-error=sequence-point;-Wno-error=parentheses;-Wno-error=implicit-fallthrough;-Wno-error=aggressive-loop-optimizations")
endif()

set(NES_EMBED_FILES
    "assets/index.html"
    "assets/tv.html"
    "assets/stream.html"
    "assets/jsnes.min.js"
)
# Optional built-in ROM. The public repo ships without one; add
# main/assets/contra.nes locally to enable it in a private build.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/contra.nes")
    list(APPEND NES_EMBED_FILES "assets/contra.nes")
    set(NES_HAS_BUILTIN ON)
else()
    set(NES_HAS_BUILTIN OFF)
endif()

idf_component_register(
    SRCS ${APP_SOURCES} ${NES_CORE_SOURCES}
    INCLUDE_DIRS
        "."
        "board"
        "display"
        "audio"
        "input"
        "rom"
        "nes"
    EMBED_FILES ${NES_EMBED_FILES}
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_spi
        esp_lcd
        esp_hid
        bt
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_server
        fatfs
        esp_codec_dev
)

# InfoNES is old C++; pin the component to GNU C++23 (IDF 6 defaults to a
# newer dialect).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
endif()

# Headless build: no local display/audio/XL9555.
if(NES_HEADLESS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NES_HEADLESS=1)
endif()

if(NES_HAS_BUILTIN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NES_HAS_BUILTIN=1)
endif()
