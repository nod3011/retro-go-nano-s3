#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes/nes.h"
#include "nes/state.h"
#include "nes_palettes.h"
#include "nofrendo.h"
#include "shared.h"

// --- GLOBALS
static rg_app_t *app;
static nes_t *nes;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static int currentBufferIndex = 0;
static bool nofrendo_running = false;
static bool slowFrame = false;

static const char *SETTING_PALETTE = "palette";

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
  slowFrame = vidbuf && !rg_display_sync(false);

  currentUpdate->width = NES_SCREEN_WIDTH;
  currentUpdate->height = NES_SCREEN_HEIGHT;
  currentUpdate->offset = NES_SCREEN_OVERDRAW;

  rg_display_submit(currentUpdate, RG_DISPLAY_WRITE_NOSYNC);
}

static void update_palette(nespal_t palette_type) {
  RG_LOGI("Updating palette to type %d\n", palette_type);
  uint16_t *pal = nofrendo_buildpalette(palette_type, 16);
  if (!pal) {
    RG_LOGE("Failed to build palette %d\n", palette_type);
    return;
  }

  for (int i = 0; i < 256; i++) {
    uint16_t color = (pal[i] >> 8) | (pal[i] << 8);
    for (int j = 0; j < 2; j++) {
      if (updates[j] && updates[j]->palette) {
        updates[j]->palette[i] = color;
      }
    }
  }
  RG_LOGI("Palette updated. Color[0] = 0x%04X (native: 0x%04X)\n",
          updates[0]->palette[0], pal[0]);
  free(pal);
}

static rg_gui_event_t palette_selection_cb(rg_gui_option_t *option,
                                           rg_gui_event_t event) {
  const char *names[] = {"Nofrendo", "Composite", "Classic",
                         "NTSC",     "PVM",       "Smooth"};
  const char *config_ns = app ? app->configNs : NS_APP;
  int palette =
      (int)rg_settings_get_number(config_ns, SETTING_PALETTE, NES_PALETTE_NTSC);

  if (palette < 0 || palette >= NES_PALETTE_COUNT) {
    palette = NES_PALETTE_NTSC;
    rg_settings_set_number(config_ns, SETTING_PALETTE, palette);
  }

  if (event == RG_DIALOG_INIT) {
    RG_LOGI("Palette init: %d (%s)\n", palette, names[palette]);
  } else if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT ||
             event == RG_DIALOG_ENTER || event == RG_DIALOG_SELECT) {
    if (event == RG_DIALOG_PREV)
      palette = (palette + NES_PALETTE_COUNT - 1) % NES_PALETTE_COUNT;
    else
      palette = (palette + 1) % NES_PALETTE_COUNT;

    RG_LOGI("Palette selecting: %d (%s)\n", palette, names[palette]);
    rg_settings_set_number(config_ns, SETTING_PALETTE, palette);
    rg_settings_commit();
    update_palette((nespal_t)palette);
    return RG_DIALOG_REDRAW;
  }

  if (option && option->value) {
    strncpy(option->value, names[palette], 15);
    option->value[15] = 0;
  }

  return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest) {
  *dest++ = (rg_gui_option_t){.label = "Color Palette",
                              .value = "-",
                              .flags = RG_DIALOG_FLAG_NORMAL,
                              .update_cb = palette_selection_cb};
  *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

// --- AUDIO
static void update_audio() {
  // Blocking audio submission provides the pacing for the emulation.
  // We use the buffer directly to follow the reference implementation.
  rg_audio_submit((const rg_audio_frame_t *)nes->apu->buffer,
                  nes->apu->samples_per_frame);
}

// --- MAIN
void nofrendo_main(void) {
  const rg_handlers_t handlers = {
      .loadState = &load_state_handler,
      .saveState = &save_state_handler,
      .reset = &reset_handler,
      .event = &event_handler,
      .screenshot = &screenshot_handler,
      .options = &options_handler,
  };

  app = rg_system_reinit(AUDIO_SAMPLE_RATE, &handlers, NULL);
  rg_system_set_overclock(1);

  // Initialize NES
  nes = nes_init(SYS_NES_NTSC, app->sampleRate, true, NULL);
  if (!nes) {
    RG_PANIC("Failed to initialize Nofrendo");
  }

  rg_system_set_tick_rate(nes->refresh_rate);

  nes->blit_func = blit_callback;

  // Surfaces: Use MEM_FAST for better performance.
  // Note: NES_SCREEN_PITCH is width + total overdraw
  updates[0] = rg_surface_create(NES_SCREEN_PITCH, NES_SCREEN_HEIGHT,
                                 RG_PIXEL_PAL565_BE, MEM_FAST);
  updates[1] = rg_surface_create(NES_SCREEN_PITCH, NES_SCREEN_HEIGHT,
                                 RG_PIXEL_PAL565_BE, MEM_FAST);
  currentBufferIndex = 0;
  currentUpdate = updates[currentBufferIndex];

  // Configure Nofrendo to render directly into our first buffer
  nes_setvidbuf(currentUpdate->data);

  // Build palette
  int palette =
      (int)rg_settings_get_number(NS_APP, SETTING_PALETTE, NES_PALETTE_NTSC);
  update_palette((nespal_t)palette);

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

    int buttons = 0;
    if (joystick & RG_KEY_START)
      buttons |= NES_PAD_START;
    if (joystick & RG_KEY_SELECT)
      buttons |= NES_PAD_SELECT;
    if (joystick & RG_KEY_A)
      buttons |= NES_PAD_A;
    if (joystick & RG_KEY_B)
      buttons |= NES_PAD_B;
    if (joystick & RG_KEY_UP)
      buttons |= NES_PAD_UP;
    if (joystick & RG_KEY_DOWN)
      buttons |= NES_PAD_DOWN;
    if (joystick & RG_KEY_LEFT)
      buttons |= NES_PAD_LEFT;
    if (joystick & RG_KEY_RIGHT)
      buttons |= NES_PAD_RIGHT;

    input_update(0, buttons);

    int64_t startTime = rg_system_timer();
    static int skipFrames = 0;

    bool drawFrame = !skipFrames;

    // Switch buffers and set target for next frame
    if (drawFrame) {
      currentBufferIndex = (currentBufferIndex + 1) % 2;
      currentUpdate = updates[currentBufferIndex];
      nes_setvidbuf(currentUpdate->data);
    }

    // Emulate one frame
    nes_emulate(drawFrame);

    // Tick before submitting audio/syncing
    rg_system_tick(rg_system_timer() - startTime);

    // Audio submission provides the pacing (sync)
    update_audio();

    if (skipFrames == 0) {
      int64_t elapsed = rg_system_timer() - startTime;
      if (elapsed > app->frameTime + 1500) { // Slight jitter allowed
        skipFrames = 1;
      } else if (drawFrame && slowFrame) {
        skipFrames = 1;
      }
    } else {
      skipFrames--;
    }

    rg_system_sync_frame(startTime);
  }

  save_sram();
  nes_shutdown();

  if (updates[0]) rg_surface_free(updates[0]);
  if (updates[1]) rg_surface_free(updates[1]);
  updates[0] = updates[1] = NULL;
}
