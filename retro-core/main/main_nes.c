#include "fceu.h"
#include "rom_manager.h"
#include "shared.h"
#include <fceumm.h>
#include <stdio.h>
#include <stdlib.h>
#include <streams/memory_stream.h>
#include <string.h>

#define NES_WIDTH 256
#define NES_HEIGHT 240

// --- GLOBALS
static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;

static uint8_t *nes_framebuffer = NULL;
static uint16_t palette565[256];
static uint32_t fceu_joystick;

// --- G&W COMPATIBILITY
rom_manager_t rom_mgr;

// --- SETTINGS
static int overscan = true;
static int autocrop = 0;
static int palette_idx = 0;

static const char *SETTING_AUTOCROP = "autocrop";
static const char *SETTING_OVERSCAN = "overscan";
static const char *SETTING_PALETTE = "palette";

// Implementation of FCEUSS_Save_Fs and FCEUSS_Load_Fs using memstream
static bool FCEUSS_Save_Fs(const char *path) {
  // Getting size for FCEUMM state
  size_t size = 1024 * 512; // 512KB is usually enough for NES
  uint8_t *buffer = malloc(size);
  if (!buffer)
    return false;

  memstream_set_buffer(buffer, size);
  FCEUSS_Save_Mem();
  size_t used = (size_t)memstream_get_last_size();

  bool success = rg_storage_write_file(path, buffer, used, 0);
  free(buffer);
  return success;
}

static bool FCEUSS_Load_Fs(const char *path) {
  void *buffer = NULL;
  size_t size = 0;
  if (!rg_storage_read_file(path, &buffer, &size, 0))
    return false;

  memstream_set_buffer((uint8_t *)buffer, size);
  FCEUSS_Load_Mem();

  free(buffer);
  return true;
}

static void update_audio(int32_t *samples, size_t count) {
  if (count == 0 || !samples)
    return;

  // Convert int32_t samples to int16_t for retro-go
  static rg_audio_frame_t audio_buf[2048];
  if (count > 2048)
    count = 2048;

  for (size_t i = 0; i < count; i++) {
    int16_t s = (int16_t)(samples[i]);
    audio_buf[i].left = s;
    audio_buf[i].right = s;
  }

  rg_audio_submit(audio_buf, count);
}

// --- FCEU CALLBACKS
void FCEUD_PrintError(char *c) { printf("FCEU Error: %s\n", c); }
void FCEUD_Message(char *s) { printf("FCEU Message: %s\n", s); }
void FCEUD_DispMessage(enum retro_log_level level, unsigned duration,
                       const char *str) {
  printf("FCEU Log [%d]: %s\n", level, str);
}

void FCEUD_SetPalette(uint8 index, uint8 r, uint8 g, uint8 b) {
  palette565[index] = C_RGB(r, g, b);
}

// --- HANDLERS
static void event_handler(int event, void *arg) {
  if (event == RG_EVENT_REDRAW) {
    rg_display_clear(C_BLACK);
  }
}

static bool screenshot_handler(const char *filename, int width, int height) {
  return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename) {
  return FCEUSS_Save_Fs(filename);
}

static bool load_state_handler(const char *filename) {
  return FCEUSS_Load_Fs(filename);
}

static bool reset_handler(bool hard) {
  if (hard)
    FCEUI_PowerNES();
  else
    FCEUI_ResetNES();
  return true;
}

// --- GUI CALLBACKS
static rg_gui_event_t palette_update_cb(rg_gui_option_t *option,
                                        rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    return RG_DIALOG_REDRAW;
  }
  strcpy(option->value, "Default");
  return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest) {
  *dest++ = (rg_gui_option_t){0, _("Palette"), "-", RG_DIALOG_FLAG_NORMAL,
                              &palette_update_cb};
  *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

// --- MAIN
void nes_main(void) {
  const rg_handlers_t handlers = {
      .loadState = &load_state_handler,
      .saveState = &save_state_handler,
      .reset = &reset_handler,
      .event = &event_handler,
      .screenshot = &screenshot_handler,
      .options = &options_handler,
  };

  app = rg_system_reinit(AUDIO_SAMPLE_RATE, &handlers, NULL);

  updates[0] =
      rg_surface_create(NES_WIDTH, NES_HEIGHT, RG_PIXEL_565_BE, MEM_FAST);
  updates[1] =
      rg_surface_create(NES_WIDTH, NES_HEIGHT, RG_PIXEL_565_BE, MEM_FAST);
  currentUpdate = updates[0];

  if (!nes_framebuffer) {
    // 256x312 for Dendy/PAL support
    nes_framebuffer = rg_alloc(NES_WIDTH * 312, MEM_SLOW);
  }
  XBuf = nes_framebuffer;

  if (!FCEUI_Initialize()) {
    RG_PANIC("FCEUI_Initialize failed");
  }

  void *rom_data = NULL;
  size_t rom_size = 0;
  if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size, 0)) {
    RG_PANIC("Failed to read ROM");
  }

  if (!FCEUI_LoadGame(app->romPath, rom_data, rom_size, NULL)) {
    RG_PANIC("FCEUI_LoadGame failed");
  }

  FCEUI_Sound(app->sampleRate);
  FCEUI_SetInput(0, SI_GAMEPAD, &fceu_joystick, 0);

  if (app->bootFlags & RG_BOOT_RESUME) {
    rg_emu_load_state(app->saveSlot);
  }

  rg_system_set_tick_rate(60);

  while (true) {
    uint32_t joystick = rg_input_read_gamepad();

    if (joystick & (RG_KEY_MENU | RG_KEY_OPTION)) {
      if (joystick & RG_KEY_MENU)
        rg_gui_game_menu();
      else
        rg_gui_options_menu();
    }

    uint32_t input_buf = 0;
    if (joystick & RG_KEY_UP)
      input_buf |= JOY_UP;
    if (joystick & RG_KEY_DOWN)
      input_buf |= JOY_DOWN;
    if (joystick & RG_KEY_LEFT)
      input_buf |= JOY_LEFT;
    if (joystick & RG_KEY_RIGHT)
      input_buf |= JOY_RIGHT;
    if (joystick & RG_KEY_A)
      input_buf |= JOY_A;
    if (joystick & JOY_B)
      input_buf |= JOY_B;
    if (joystick & RG_KEY_START)
      input_buf |= JOY_START;
    if (joystick & RG_KEY_SELECT)
      input_buf |= JOY_SELECT;
    fceu_joystick = input_buf;

    int64_t startTime = rg_system_timer();

    uint8_t *gfx = NULL;
    int32_t *sound = NULL;
    int32_t sound_samples = 0;

    FCEUI_Emulate(&gfx, &sound, &sound_samples, 0);

    if (gfx && currentUpdate && currentUpdate->data) {
      currentUpdate = updates[currentUpdate == updates[0]];
      uint16_t *dst = (uint16_t *)currentUpdate->data;
      // Use the actual height for the copy loop to avoid overflow
      // normal_scanlines is updated by FCEU_ResetVidSys called within
      // FCEUI_LoadGame
      extern unsigned normal_scanlines;
      int limit = NES_WIDTH * (normal_scanlines < NES_HEIGHT ? normal_scanlines
                                                             : NES_HEIGHT);
      for (int i = 0; i < limit; i++) {
        dst[i] = palette565[gfx[i]];
      }
      rg_display_submit(currentUpdate, 0);
    }

    update_audio(sound, sound_samples);

    rg_system_tick(rg_system_timer() - startTime);
  }
}
