# Retro-Go AI Coding Instructions

## Architecture Overview
Retro-Go is an ESP-IDF-based multi-application firmware for retro gaming on ESP32 devices. The `components/retro-go/` library provides unified APIs for audio, display, input, storage, and GUI across supported devices (ODROID-GO, MRGC-G32, etc.).

- **Core Components**: `rg_audio`, `rg_display`, `rg_input`, `rg_storage`, `rg_gui` - handle hardware abstraction
- **App Structure**: Each emulator (fmsx, gwenesis, prboom-go) is a separate ESP-IDF app with `main/main.c` and `CMakeLists.txt`
- **Launcher**: Manages app selection and settings; bundled emulators (NES, PCE, etc.) are in `retro-core` for size optimization
- **Data Flow**: Emulators render to `rg_surface_t`, use `rg_display_write()` for output; audio via `rg_audio_submit()`

## Build Workflow
Use `rg_tool.py` instead of `idf.py` for multi-app management:
- `python rg_tool.py build-fw` - Build full firmware image
- `python rg_tool.py flash <app>` - Flash individual app for development
- `python rg_tool.py run <app>` - Flash and monitor app
- Set `RG_TOOL_TARGET` and `RG_TOOL_PORT` environment variables

Requires ESP-IDF 4.4+ with patches from `tools/patches/` applied.

## Coding Patterns
- **Integration**: Include `<rg_system.h>`; initialize with `rg_system_init()`; main loop calls `rg_system_tick()`
- **Display**: Use `rg_display_write(surface, flags)`; scaling/filtering via `rg_display_set_scaling()`
- **Input**: Poll with `rg_input_read_gamepad()`; buttons as bitflags (RG_KEY_*)
- **Storage**: Paths via `rg_storage_get_path(type, filename)`; saves auto-managed
- **Localization**: Wrap strings in `_(...)`; translations in `components/retro-go/translations.h`
- **Memory**: Use `rg_alloc()` for managed heap; avoid ESP-IDF direct heap calls

## Development Conventions
- **File Structure**: Apps in top-level dirs (fmsx/, launcher/); shared code in `components/retro-go/`
- **Configuration**: Per-app `sdkconfig`; device configs in `components/retro-go/targets/`
- **Debugging**: Crash logs saved to `/sd/crash.log`; resolve with `xtensa-esp32-elf-addr2line -ifCe <app>/build/<app>.elf`
- **Assets**: Launcher images in `themes/default/`; regenerate with `python tools/gen_images.py`
- **Emulator Ports**: Keep ESP32-specific code in `main.c`; reuse emulator cores with minimal changes

## Key Files
- `BUILDING.md` - Detailed build/porting instructions
- `components/retro-go/rg_system.h` - Main API header
- `rg_tool.py` - Build script with all commands
- `PORTING.md` - Device porting guide
- Example app: `fmsx/main/main.c` - Shows integration pattern