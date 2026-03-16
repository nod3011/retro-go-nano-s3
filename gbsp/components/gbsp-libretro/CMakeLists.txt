set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ". libretro/libretro-common/include")
set(COMPONENT_REQUIRES "retro-go")

# Add core source files
list(APPEND COMPONENT_SRCS 
    "main.c"
    "sound.c"
    "input.c"
    "savestate.c"
    "gba_memory.c"
    "video.cpp"
    "cpu.cpp"
    "serial.c"
    "cheats.c"
    "rfu.c"
    "gbp.c"
)

# Add ESP32-S3 specific source files
if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND COMPONENT_SRCS 
        "esp32s3_opt.c"
        "esp32s3_dualcore.c" 
        "esp32s3_integration.c"
    )
    list(APPEND COMPONENT_COMPILE_DEFINITIONS "ESP32=1" "ENABLE_DUAL_CORE=1")
endif()

# Add missing functions file
list(APPEND COMPONENT_SRCS "gbsp_missing_functions.c")

# Export symbols for main component
set(COMPONENT_EXPORT_SYMBOLS "${CMAKE_CURRENT_LIST_DIR}/gbsp_libretro.exports")

register_component()
rg_setup_compile_options(
    -O3
    -fomit-frame-pointer -ffast-math
    -DOVERCLOCK_60FPS
    -DROM_BUFFER_SIZE=16
    -Wno-unused-variable
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
    -Wno-error=unused-label
    -Wno-unused-label
)
