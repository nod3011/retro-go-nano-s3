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
// Implementation of FCEUSS_Save_Fs and FCEUSS_Load_Fs using memstream
static bool FCEUSS_Save_Fs(const char *path) {
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

static CartInfo *get_cart_info(void) {
  if (iNESCart.mapper >= 0 || iNESCart.PRGRomSize > 0)
    return &iNESCart;
  if (UNIFCart.PRGRomSize > 0)
    return &UNIFCart;
  return NULL;
}

static bool load_sram(void) {
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, NULL);
  if (!path)
    return false;

  void *buffer = NULL;
  size_t size = 0;
  if (!rg_storage_read_file(path, &buffer, &size, 0))
    return false;

  CartInfo *cart = get_cart_info();
  if (!cart) {
    free(buffer);
    return false;
  }

  bool loaded = false;
  for (int i = 0; i < 4; i++) {
    if (cart->SaveGame[i] && cart->SaveGameLen[i] > 0) {
      size_t len = cart->SaveGameLen[i];
      if (size >= len) {
        memcpy(cart->SaveGame[i], buffer, len);
        loaded = true;
        break; // Only support first slot for now
      }
    }
  }

  free(buffer);
  return loaded;
}

static bool save_sram(void) {
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, NULL);
  CartInfo *cart = get_cart_info();
  if (!path || !cart)
    return false;

  for (int i = 0; i < 4; i++) {
    if (cart->SaveGame[i] && cart->SaveGameLen[i] > 0) {
      return rg_storage_write_file(path, cart->SaveGame[i],
                                   cart->SaveGameLen[i], 0);
    }
  }
  return false;
}

static void update_audio(int32_t *samples, size_t count) {
  if (count == 0 || !samples)
    return;

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
  } else if (event == RG_EVENT_SHUTDOWN || event == RG_EVENT_SLEEP) {
    save_sram();
  }
}

static bool screenshot_handler(const char *filename, int width, int height) {
  return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename) {
  save_sram(); // Good time to save SRAM too
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
static const char *pal_names[] = {"Default", "Sony CRT", "WaveBeam", "Smooth"};
static rg_gui_event_t palette_update_cb(rg_gui_option_t *option,
                                        rg_gui_event_t event) {
  if (event == RG_DIALOG_INIT) {
    palette_idx = rg_settings_get_number(NS_APP, SETTING_PALETTE, 0);
  }
  if (event == RG_DIALOG_PREV) {
    palette_idx = (palette_idx + 3) % 4;
  } else if (event == RG_DIALOG_NEXT) {
    palette_idx = (palette_idx + 1) % 4;
  } else if (event == RG_DIALOG_VOID) {
    // No action
  } else {
    return RG_DIALOG_VOID;
  }

  rg_settings_set_number(NS_APP, SETTING_PALETTE, palette_idx);
  strcpy(option->value, pal_names[palette_idx]);
  // TODO: Call FCEUI_SetPaletteArray if we have external palette data
  // For now, we just update the label. Active palette switching needs more
  // work.
  return RG_DIALOG_REDRAW;
}

static void options_handler(rg_gui_option_t *dest) {
  *dest++ = (rg_gui_option_t){0, _("Palette"), "-", RG_DIALOG_FLAG_NORMAL,
                              &palette_update_cb};
  *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

// FDS Functions from fds.h
extern bool FCEU_FDSIsDiskInserted();
extern void FCEU_FDSInsert(int oride);
extern void FCEU_FDSEject(void);
extern void FCEU_FDSSelect(void);
extern void FCEU_FDSSelect_previous(void);

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

  load_sram();

  // Region detection
  int slstart, slend;
  int is_pal = FCEUI_GetCurrentVidSystem(&slstart, &slend);
  rg_system_set_tick_rate(is_pal ? 50 : 60);
  RG_LOGI("Detected %s region (%d lines)\n", is_pal ? "PAL" : "NTSC",
          is_pal ? 312 : 262);

  // Use normal_scanlines (updated by LoadGame) for surface height
  extern unsigned normal_scanlines;
  int surface_height = (normal_scanlines > 0 && normal_scanlines <= 312)
                           ? normal_scanlines
                           : NES_HEIGHT;

  updates[0] =
      rg_surface_create(NES_WIDTH, surface_height, RG_PIXEL_565_LE, MEM_FAST);
  updates[1] =
      rg_surface_create(NES_WIDTH, surface_height, RG_PIXEL_565_LE, MEM_FAST);
  currentUpdate = updates[0];

  FCEUI_Sound(app->sampleRate);
  FCEUI_SetInput(0, SI_GAMEPAD, &fceu_joystick, 0);

  if (app->bootFlags & RG_BOOT_RESUME) {
    rg_emu_load_state(app->saveSlot);
  }

  int64_t last_sram_save = rg_system_timer();

  static uint32_t joystick_old = 0;
  static bool menu_cancelled = false;
  static bool menu_pressed = false;
  static bool turbo_a_toggled = false;
  static bool turbo_b_toggled = false;
  static int turbo_counter = 0;

  while (true) {
    uint32_t joystick = rg_input_read_gamepad();
    uint32_t joystick_down = joystick & ~joystick_old;
    uint32_t input_buf = 0;
    turbo_counter++;

    // MENU button modifier logic
    if (joystick & RG_KEY_MENU) {
      // Toggle Turbo A
      if (joystick_down & RG_KEY_A) {
        turbo_a_toggled = !turbo_a_toggled;
        RG_LOGI("Turbo A: %s\n", turbo_a_toggled ? "ON" : "OFF");
      }
      // Toggle Turbo B
      if (joystick_down & RG_KEY_B) {
        turbo_b_toggled = !turbo_b_toggled;
        RG_LOGI("Turbo B: %s\n", turbo_b_toggled ? "ON" : "OFF");
      }
      // FDS: Automated Quick Swap (Eject -> Next -> Insert)
      if (joystick_down & RG_KEY_START) {
        FCEUI_FDSEject();
        FCEUI_FDSSelect();
        FCEUI_FDSInsert(0);
        RG_LOGI("FDS: Automated Side Swap\n");
      }
      // FDS: Manual Insert
      if (joystick_down & RG_KEY_UP) {
        FCEUI_FDSInsert(0);
        RG_LOGI("FDS: Disk Inserted\n");
      }
      // FDS: Manual Eject
      if (joystick_down & RG_KEY_DOWN) {
        FCEUI_FDSEject();
        RG_LOGI("FDS: Disk Ejected\n");
      }
      // FDS: Change Side (Next)
      if (joystick_down & RG_KEY_SELECT) {
        FCEUI_FDSSelect();
        RG_LOGI("FDS: Switched Side\n");
      }

      // If any other button is pressed while MENU is held, cancel the menu
      // trigger
      if (joystick & ~RG_KEY_MENU) {
        menu_cancelled = true;
      }
      menu_pressed = true;
    } else {
      // Handle Menu release
      if (joystick_old & RG_KEY_MENU) {
        if (!menu_cancelled) {
          save_sram();
          rg_gui_game_menu();
        }
        menu_cancelled = false;
      }
      menu_pressed = false;
    }

    // Process Basic Inputs
    if (!menu_pressed) {
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
      if (joystick & RG_KEY_B)
        input_buf |= JOY_B;
      if (joystick & RG_KEY_START)
        input_buf |= JOY_START;
      if (joystick & RG_KEY_SELECT)
        input_buf |= JOY_SELECT;
    }

    // Apply Turbo Toggles (Continuous auto-fire)
    if (turbo_counter & 4) {
      if (turbo_a_toggled)
        input_buf |= JOY_A;
      if (turbo_b_toggled)
        input_buf |= JOY_B;
    }

    if (joystick & RG_KEY_OPTION) {
      save_sram();
      rg_gui_options_menu();
    }

    joystick_old = joystick;
    fceu_joystick = input_buf;

    int64_t startTime = rg_system_timer();

    uint8_t *gfx = NULL;
    int32_t *sound = NULL;
    int32_t sound_samples = 0;

    FCEUI_Emulate(&gfx, &sound, &sound_samples, 0);

    if (gfx && currentUpdate && currentUpdate->data) {
      currentUpdate = updates[currentUpdate == updates[0]];
      uint16_t *dst = (uint16_t *)currentUpdate->data;
      int limit = NES_WIDTH * currentUpdate->height;
      for (int i = 0; i < limit; i++) {
        dst[i] = palette565[gfx[i]];
      }
      rg_display_submit(currentUpdate, 0);
    }

    update_audio(sound, sound_samples);

    rg_system_tick(rg_system_timer() - startTime);

    // Auto-save SRAM every 30 seconds
    if (rg_system_timer() - last_sram_save > 30000000) { // 30s
      save_sram();
      last_sram_save = rg_system_timer();
    }
  }
}
