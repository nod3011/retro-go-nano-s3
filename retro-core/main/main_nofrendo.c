#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes/nes.h"
#include "nes/state.h"
#include "nofrendo.h"

// --- GLOBALS
static rg_app_t *app;
static nes_t *nes;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static uint16_t *nes_palette_16 = NULL;
static uint8_t *nofrendo_vidbuf = NULL;
static bool nofrendo_running = false;

#define NES_WIDTH 256
#define NES_HEIGHT 240

static bool save_sram(void);

static void event_handler(int event, void *arg) {
  if (event == RG_EVENT_REDRAW) {
    rg_display_clear(C_BLACK);
  } else if (event == RG_EVENT_SHUTDOWN || event == RG_EVENT_SLEEP) {
    save_sram();
  }
}

static bool load_sram(void) {
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
  if (path) {
    rom_loadsram(path);
    free(path);
    return true;
  }
  return false;
}

static bool save_sram(void) {
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
  if (path) {
    rg_storage_mkdir(rg_dirname(path));
    rom_savesram(path);
    free(path);
    return true;
  }
  return false;
}

static bool screenshot_handler(const char *filename, int width, int height) {
  return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename) {
  return state_save(filename) == 0;
}

static bool load_state_handler(const char *filename) {
  return state_load(filename) == 0;
}

static bool reset_handler(bool hard) {
  nes_reset(hard);
  return true;
}

// --- RENDERING
static void blit_callback(uint8 *vidbuf) {
  // If we follow the original logic, NOFRENDO renders directly to our surface
  // buffer. We just need to submit it.
  currentUpdate->width = NES_SCREEN_WIDTH;
  currentUpdate->height = NES_SCREEN_HEIGHT;
  currentUpdate->offset = NES_SCREEN_OVERDRAW; // Skip horizontal overdraw

  rg_display_submit(currentUpdate, RG_DISPLAY_WRITE_NOSYNC);
}

// --- AUDIO
static void update_audio() {
  // Blocking audio submission provides the pacing for the emulation
  rg_audio_submit((void *)nes->apu->buffer, nes->apu->samples_per_frame);
}

// --- MAIN
void nofrendo_main(void) {
  const rg_handlers_t handlers = {
      .loadState = &load_state_handler,
      .saveState = &save_state_handler,
      .reset = &reset_handler,
      .event = &event_handler,
      .screenshot = &screenshot_handler,
  };

  app = rg_system_reinit(32000, &handlers, NULL);
  rg_system_set_overclock(1); // MED level is enough for Nofrendo

  // Initialize NES
  nes = nes_init(SYS_NES_NTSC, app->sampleRate, true, NULL);
  if (!nes) {
    RG_PANIC("Failed to initialize Nofrendo");
  }

  nes->blit_func = blit_callback;

  // Surfaces: Use MEM_FAST for internal RAM.
  // Note: NES_SCREEN_PITCH is width + total overdraw
  updates[0] = rg_surface_create(NES_SCREEN_PITCH, NES_SCREEN_HEIGHT,
                                 RG_PIXEL_PAL565_BE, MEM_FAST);
  updates[1] = rg_surface_create(NES_SCREEN_PITCH, NES_SCREEN_HEIGHT,
                                 RG_PIXEL_PAL565_BE, MEM_FAST);
  currentUpdate = updates[0];

  // Configure Nofrendo to render directly into our first buffer
  nes_setvidbuf(currentUpdate->data);

  // Build palette
  uint16_t *pal = nofrendo_buildpalette(NES_PALETTE_NTSC, 16);
  if (pal) {
    for (int i = 0; i < 256; i++) {
      // Original good build used BE (Big Endian) for palette colors in surface
      uint16_t color = (pal[i] >> 8) | (pal[i] << 8);
      updates[0]->palette[i] = color;
      updates[1]->palette[i] = color;
    }
    free(pal);
  }

  // Clear display
  rg_display_clear(C_BLACK);

  nofrendo_running = true;
  if (nes_loadfile(app->romPath) < 0) {
    RG_PANIC("Nofrendo: Failed to load ROM");
  }

  // Good build does this for better compatibility/state
  nes_emulate(false);
  nes_emulate(false);

  if (app->bootFlags & RG_BOOT_RESUME) {
    load_state_handler(
        rg_emu_get_path(RG_PATH_SAVE_STATE + app->saveSlot, app->romPath));
  }

  load_sram();

  // Ensure SRAM directory exists
  char *sram_path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
  if (sram_path) {
    rg_storage_mkdir(rg_dirname(sram_path));
    free(sram_path);
  }

  while (nofrendo_running && !rg_system_exit_called()) {
    uint32_t joystick = rg_input_read_gamepad();

    if (joystick & (RG_KEY_MENU | RG_KEY_OPTION)) {
      save_sram();
      if (joystick & RG_KEY_MENU)
        rg_gui_game_menu();
      else
        rg_gui_options_menu();
    }

    int input = 0;
    if (joystick & RG_KEY_START)
      input |= (1 << 3);
    if (joystick & RG_KEY_SELECT)
      input |= (1 << 2);
    if (joystick & RG_KEY_A)
      input |= (1 << 0);
    if (joystick & RG_KEY_B)
      input |= (1 << 1);
    if (joystick & RG_KEY_UP)
      input |= (1 << 4);
    if (joystick & RG_KEY_DOWN)
      input |= (1 << 5);
    if (joystick & RG_KEY_LEFT)
      input |= (1 << 6);
    if (joystick & RG_KEY_RIGHT)
      input |= (1 << 7);

    input_update(0, input);

    int64_t startTime = rg_system_timer();

    // Switch buffers and set target for next frame
    currentUpdate = updates[currentUpdate == updates[0]];
    nes_setvidbuf(currentUpdate->data);

    // Emulate one frame
    nes_emulate(true);

    // Audio submission provides the pacing (sync)
    update_audio();

    // Stats
    rg_system_tick(rg_system_timer() - startTime);
  }

  save_sram();
  nes_shutdown();
}
