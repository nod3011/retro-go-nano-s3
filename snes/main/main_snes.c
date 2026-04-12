#include "shared.h"

#include <math.h>
#include <snes9x.h>

typedef struct {
  char name[16];
  struct {
    uint16_t snes9x_mask;
    uint16_t local_mask;
    uint16_t mod_mask;
  } keys[16];
} keymap_t;

// Physical buttons on Nano S3: B, A, Select, Start (+ Menu as modifier)
// Type A: A=A, B=B, Start=X, Select=Y, Menu+B=L, Menu+A=R, Menu+Start=Start, Menu+Select=Select
// Type B: A=B, B=Y, Start=A, Select=X, Menu+B=L, Menu+A=R, Menu+Start=Start, Menu+Select=Select
static const keymap_t KEYMAPS[] = {
    {"Type A",
     {
         {SNES_A_MASK, RG_KEY_A, 0},
         {SNES_B_MASK, RG_KEY_B, 0},
         {SNES_X_MASK, RG_KEY_START, 0},
         {SNES_Y_MASK, RG_KEY_SELECT, 0},
         {SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
         {SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
         {SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
         {SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
         {SNES_UP_MASK, RG_KEY_UP, 0},
         {SNES_DOWN_MASK, RG_KEY_DOWN, 0},
         {SNES_LEFT_MASK, RG_KEY_LEFT, 0},
         {SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
     }},
    {"Type B",
     {
         {SNES_A_MASK, RG_KEY_START, 0},
         {SNES_B_MASK, RG_KEY_A, 0},
         {SNES_X_MASK, RG_KEY_SELECT, 0},
         {SNES_Y_MASK, RG_KEY_B, 0},
         {SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
         {SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
         {SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
         {SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
         {SNES_UP_MASK, RG_KEY_UP, 0},
         {SNES_DOWN_MASK, RG_KEY_DOWN, 0},
         {SNES_LEFT_MASK, RG_KEY_LEFT, 0},
         {SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
     }},
    // Custom: keys[0..7] are always overwritten by update_keymap() from current_custom_mapping.
    // Only D-pad (keys[8..11]) is used as-is from this template.
    {"Custom",
     {
         {SNES_A_MASK, RG_KEY_A, 0},
         {SNES_B_MASK, RG_KEY_B, 0},
         {SNES_X_MASK, RG_KEY_START, 0},
         {SNES_Y_MASK, RG_KEY_SELECT, 0},
         {SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
         {SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
         {SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
         {SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
         {SNES_UP_MASK, RG_KEY_UP, 0},
         {SNES_DOWN_MASK, RG_KEY_DOWN, 0},
         {SNES_LEFT_MASK, RG_KEY_LEFT, 0},
         {SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
     }},
};

typedef struct {
  uint32_t magic;
  int keymap_id;
  int overclock;              // Added per-game overclock
  uint16_t custom_mapping[8]; // index into SNES_BUTTONS
} snes_config_t;

static const struct {
  const char *name;
  uint16_t local_mask;
  uint16_t mod_mask;
} PHYSICAL_BUTTONS[] = {
    {"B", RG_KEY_B, 0},
    {"A", RG_KEY_A, 0},
    {"Select", RG_KEY_SELECT, 0},
    {"Start", RG_KEY_START, 0},
    {"Menu+B", RG_KEY_B, RG_KEY_MENU},
    {"Menu+A", RG_KEY_A, RG_KEY_MENU},
    {"Menu+Select", RG_KEY_SELECT, RG_KEY_MENU},
    {"Menu+Start", RG_KEY_START, RG_KEY_MENU},
};

// Default Custom mapping: B, A, Y, X, L, R, Select, Start (Matching Type A logical mapping)
static uint16_t current_custom_mapping[8] = {15, 7, 14, 6, 5, 4, 13, 12};

static const size_t KEYMAPS_COUNT = (sizeof(KEYMAPS) / sizeof(keymap_t));

// Indexed by bit position in the SNES joypad word. Bits 0-3 are unused in snes9x.
static const char *SNES_BUTTONS[] = {
    "None",  "---",  "---",  "---",  "R",     "L",      "X", "A",
    "Right", "Left", "Down", "Up",   "Start", "Select", "Y", "B"};

#define AUDIO_LOW_PASS_RANGE ((60 * 65536) / 100)

static rg_app_t *app;
static rg_surface_t *updates[3];
static rg_surface_t *currentUpdate;
static int currentBufferIndex = 0;
static rg_audio_sample_t *audioBuffer;

static bool apu_enabled = true;
static bool lowpass_filter = false;

static int keymap_id = 0;
static keymap_t keymap;

static const char *SETTING_APU_EMULATION = "apu";
// --- MAIN

static void update_keymap(int id) {
  keymap_id = id % KEYMAPS_COUNT;
  keymap = KEYMAPS[keymap_id];

  if (keymap_id == 2) { // Custom
    for (int i = 0; i < 8; i++) {
      int snes_idx = current_custom_mapping[i];
      keymap.keys[i].snes9x_mask = (snes_idx > 0) ? (1 << snes_idx) : 0;
      keymap.keys[i].local_mask = PHYSICAL_BUTTONS[i].local_mask;
      keymap.keys[i].mod_mask = PHYSICAL_BUTTONS[i].mod_mask;
    }
    // D-Pad stays as defined in KEYMAPS[2] (Standard D-Pad)
  }
}

static void save_config() {
  char path[RG_PATH_MAX];
  snprintf(path, sizeof(path), "%s/snes/%s.cfg", RG_BASE_PATH_CONFIG,
           rg_basename(app->romPath));

  rg_storage_mkdir(rg_dirname(path));

  snes_config_t cfg = {
      .magic = 0x534E4553,
      .keymap_id = keymap_id,
      .overclock = rg_system_get_overclock(),
  };
  memcpy(cfg.custom_mapping, current_custom_mapping, sizeof(current_custom_mapping));

  if (rg_storage_write_file(path, &cfg, sizeof(cfg), 0)) {
    RG_LOGI("Config saved to %s (OC:%d)\n", path, cfg.overclock);
  }
}

static void load_config() {
  char path[RG_PATH_MAX];
  snprintf(path, sizeof(path), "%s/snes/%s.cfg", RG_BASE_PATH_CONFIG,
           rg_basename(app->romPath));

  void *data = NULL;
  size_t size = 0;
  if (rg_storage_read_file(path, &data, &size, 0)) {
    if (size >= sizeof(snes_config_t)) {
      snes_config_t *cfg = (snes_config_t *)data;
      if (cfg->magic == 0x534E4553) {
        keymap_id = cfg->keymap_id;
        memcpy(current_custom_mapping, cfg->custom_mapping,
               sizeof(current_custom_mapping));
        if (cfg->overclock >= 0 && cfg->overclock <= 3) {
          rg_system_set_overclock(cfg->overclock);
        }
        RG_LOGI("Config loaded from %s (OC:%d)\n", path, cfg->overclock);
      }
    }
    free(data);
  }
  update_keymap(keymap_id);
}

static bool screenshot_handler(const char *filename, int width, int height) {
  return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename) {
  return S9xSaveState(filename);
}

static bool load_state_handler(const char *filename) {
  return S9xLoadState(filename);
}

static void save_sram(bool force) {
  if (app->romPath && (CPU.SRAMModified || force)) {
    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);

    char dirname[RG_PATH_MAX];
    const char *dir = rg_dirname(path);
    if (dir) {
      strcpy(dirname, dir);
      rg_storage_mkdir(dirname);
    }

    if (rg_storage_write_file(path, Memory.SRAM, SRAM_SIZE, 0)) {
      CPU.SRAMModified = false;
    } else {
      RG_LOGE("Failed to save SRAM to: %s\n", path);
    }
    free(path);
  }
}

static void load_sram() {
  if (app->romPath && Memory.SRAM) {
    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    size_t size = 0;
    void *data = NULL;
    if (rg_storage_read_file(path, &data, &size, 0)) {
      memcpy(Memory.SRAM, data, size > SRAM_SIZE ? SRAM_SIZE : size);
      free(data);
    }
    free(path);
  }
}

static bool reset_handler(bool hard) {
  save_sram(true);
  S9xReset();
  return true;
}

static void event_handler(int event, void *arg) {
  if (event == RG_EVENT_REDRAW) {
    rg_display_submit(currentUpdate, RG_DISPLAY_WRITE_NOSYNC);
  } else if (event == RG_EVENT_SHUTDOWN || event == RG_EVENT_SLEEP) {
    save_sram(true);
  }
}

static rg_gui_event_t apu_toggle_cb(rg_gui_option_t *option,
                                    rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    apu_enabled = !apu_enabled;
    rg_settings_set_number(NS_APP, SETTING_APU_EMULATION, apu_enabled);
  }

  strcpy(option->value, apu_enabled ? _("On") : _("Off"));

  return RG_DIALOG_VOID;
}

static rg_gui_event_t lowpass_filter_cb(rg_gui_option_t *option,
                                        rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    lowpass_filter = !lowpass_filter;
    rg_settings_set_number(NS_APP, "lowpass", lowpass_filter);
  }

  strcpy(option->value, lowpass_filter ? _("On") : _("Off"));

  return RG_DIALOG_VOID;
}


static rg_gui_event_t sub_btn_mapping_cb(rg_gui_option_t *option,
                                         rg_gui_event_t event) {
  int i = (int)option->arg;
  uint16_t *val = &current_custom_mapping[i];

  // Target SNES buttons: None (0), A(7), B(15), X(6), Y(14), L(5), R(4), Start(12), Select(13)
  static const uint8_t TARGETS[] = {0, 7, 15, 6, 14, 5, 4, 12, 13};
  int current_idx = 0;
  for (int j = 0; j < 9; j++) {
    if (TARGETS[j] == *val) {
      current_idx = j;
      break;
    }
  }

  if (event == RG_DIALOG_PREV)
    current_idx = (current_idx + 8) % 9;
  if (event == RG_DIALOG_NEXT)
    current_idx = (current_idx + 1) % 9;

  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    *val = TARGETS[current_idx];
    update_keymap(keymap_id);
  }

  strcpy(option->value, SNES_BUTTONS[*val]);

  return RG_DIALOG_VOID;
}


static rg_gui_event_t btn_mapping_cb(rg_gui_option_t *option,
                                     rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    if (keymap_id != 2) {
      rg_gui_alert(_("Notice"), _("Please select 'Custom' profile first."));
      return RG_DIALOG_VOID;
    }
    rg_gui_option_t options[9];
    for (int i = 0; i < 8; i++) {
      options[i] = (rg_gui_option_t){i, PHYSICAL_BUTTONS[i].name, "-",
                                     RG_DIALOG_FLAG_NORMAL, &sub_btn_mapping_cb};
    }
    options[8] = (rg_gui_option_t)RG_DIALOG_END;

    rg_gui_dialog(option->label, options, 0);
    save_config(); // Auto-save on exit
    return RG_DIALOG_REDRAW;
  }
  return RG_DIALOG_VOID;
}

static rg_gui_event_t change_keymap_cb(rg_gui_option_t *option,
                                       rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    if (event == RG_DIALOG_PREV && --keymap_id < 0)
      keymap_id = KEYMAPS_COUNT - 1;
    if (event == RG_DIALOG_NEXT && ++keymap_id > KEYMAPS_COUNT - 1)
      keymap_id = 0;
    update_keymap(keymap_id);
    save_config(); // Save immediately on change
    return RG_DIALOG_REDRAW;
  }

  if (keymap_id == 0) strcpy(option->value, "A");
  else if (keymap_id == 1) strcpy(option->value, "B");
  else strcpy(option->value, "Custom");

  return RG_DIALOG_VOID;
}

static rg_gui_event_t menu_keymap_cb(rg_gui_option_t *option,
                                     rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    rg_gui_option_t options[3];
    options[0] = (rg_gui_option_t){-1, _("Profile Type"), "-", RG_DIALOG_FLAG_NORMAL,
                                   &change_keymap_cb};
    options[1] = (rg_gui_option_t){0, _("Customize Buttons"), "...",
                                   RG_DIALOG_FLAG_NORMAL, &btn_mapping_cb};
    options[2] = (rg_gui_option_t)RG_DIALOG_END;

    rg_gui_dialog(option->label, options, 0);
    return RG_DIALOG_REDRAW;
  }
  return RG_DIALOG_VOID;
}

bool S9xInitDisplay(void) {
  GFX.Pitch = SNES_WIDTH * 2;
  GFX.ZPitch = SNES_WIDTH;
  GFX.Screen = currentUpdate->data;

  // SubScreen is large (~122KB) so it lives in PSRAM.
  GFX.SubScreen  = rg_alloc(GFX.Pitch  * SNES_HEIGHT_EXTENDED, MEM_SLOW);

  // ZBuffer and SubZBuffer are each ~30KB and accessed every pixel during rendering.
  // Keeping them in Internal SRAM (MEM_FAST) is critical for performance at all OC levels.
  // The memset below (not the memory location) is what fixes the graphics glitch:
  // SNES9x needs zeroed Z-Buffers to correctly resolve sprite/layer priority.
  GFX.ZBuffer    = rg_alloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED, MEM_FAST);
  GFX.SubZBuffer = rg_alloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED, MEM_FAST);

  if (GFX.SubScreen)  memset(GFX.SubScreen,  0, GFX.Pitch  * SNES_HEIGHT_EXTENDED);
  if (GFX.ZBuffer)    memset(GFX.ZBuffer,    0, GFX.ZPitch * SNES_HEIGHT_EXTENDED);
  if (GFX.SubZBuffer) memset(GFX.SubZBuffer, 0, GFX.ZPitch * SNES_HEIGHT_EXTENDED);

  return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void) {}

uint32_t S9xReadJoypad(int32_t port) {
  if (port != 0)
    return 0;

  uint32_t joystick = rg_input_read_gamepad();
  uint32_t joypad = 0;
  uint32_t consumed = 0;

  // First pass: evaluate and trigger combos, marking local keys as consumed
  for (int i = 0; i < RG_COUNT(keymap.keys); ++i) {
    uint32_t local = keymap.keys[i].local_mask;
    uint32_t mod = keymap.keys[i].mod_mask;
    if (mod != 0 && (joystick & mod) == mod && (joystick & local) == local) {
      joypad |= keymap.keys[i].snes9x_mask;
      consumed |= local;
    }
  }

  // Second pass: evaluate unmodified keys only if they were not consumed by a
  // combo
  for (int i = 0; i < RG_COUNT(keymap.keys); ++i) {
    uint32_t local = keymap.keys[i].local_mask;
    uint32_t mod = keymap.keys[i].mod_mask;
    if (mod == 0 && (joystick & local) == local && !(consumed & local)) {
      joypad |= keymap.keys[i].snes9x_mask;
    }
  }

  return joypad;
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y,
                          uint32_t *buttons) {
  return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons) {
  return false;
}

bool JustifierOffscreen(void) { return true; }

void JustifierButtons(uint32_t *justifiers) { (void)justifiers; }

#ifdef USE_BLARGG_APU
static void S9xAudioCallback(void) {
  S9xFinalizeSamples();
  size_t available_samples = S9xGetSampleCount();
  S9xMixSamples((void *)audioBuffer, available_samples);
  rg_audio_submit(audioBuffer, available_samples >> 1);
}
#endif

static void options_handler(rg_gui_option_t *dest) {
  *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL,
                              &apu_toggle_cb};
  *dest++ = (rg_gui_option_t){0, _("Audio filter"), "-", RG_DIALOG_FLAG_NORMAL,
                              &lowpass_filter_cb};
  *dest++ = (rg_gui_option_t){0, _("Controls"), "...", RG_DIALOG_FLAG_NORMAL,
                              &menu_keymap_cb};
  *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

void app_main(void) {
  const rg_handlers_t handlers = {
      .loadState = &load_state_handler,
      .saveState = &save_state_handler,
      .reset = &reset_handler,
      .screenshot = &screenshot_handler,
      .event = &event_handler,
      .options = &options_handler,
  };
  app = rg_system_reinit(AUDIO_SAMPLE_RATE, &handlers, NULL);

  // Load persisted settings before applying defaults
  apu_enabled    = rg_settings_get_number(NS_APP, SETTING_APU_EMULATION, 1);
  lowpass_filter = rg_settings_get_number(NS_APP, "lowpass", 0);

  // Force Full Screen Scaling for 240x240 Nano-S3
  rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);

  // Set default overclock level (will be overridden by per-game config in load_config)
  rg_system_set_overclock(2);
  app->frameskip = 0; // Fix: rg_system_set_overclock sets frameskip to 1 internally

  for (int i = 0; i < 3; i++) {
    updates[i] = rg_surface_create(SNES_WIDTH, SNES_HEIGHT_EXTENDED,
                                   RG_PIXEL_565_LE, MEM_SLOW);
    memset(updates[i]->data, 0, updates[i]->stride * updates[i]->height);
  }
  currentBufferIndex = 0;
  currentUpdate = updates[currentBufferIndex];

  audioBuffer = (rg_audio_sample_t *)malloc(AUDIO_BUFFER_LENGTH * 8);
  if (!audioBuffer)
    RG_PANIC("Audio buffer allocation failed!");

  load_config();

  Settings.CyclesPercentage = 100;
  Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
  Settings.FrameTimePAL = 20000;
  Settings.FrameTimeNTSC = 16667;
  Settings.ControllerOption = SNES_JOYPAD;
  Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
  Settings.SoundPlaybackRate = AUDIO_SAMPLE_RATE;
  Settings.SoundInputRate = AUDIO_SAMPLE_RATE;
  Settings.DisableSoundEcho = true;
  Settings.InterpolatedSound = false;

  if (!S9xInitDisplay())
    RG_PANIC("Display init failed!");

  const char *romPath = app->romPath;
  size_t romSize =
      0x600000; // Default to 6MB if we can't determine size (e.g. ZIP)

  rg_stat_t st = rg_storage_stat(romPath);
  if (st.exists && st.is_file && !rg_extension_match(romPath, "zip")) {
    romSize = st.size;
  }

  Memory.ROM_AllocSize = romSize + 0x10000 + 0x200;
  Memory.ROM = (uint8_t *)rg_alloc(Memory.ROM_AllocSize, MEM_SLOW);

  if (!S9xInitMemory())
    RG_PANIC("Memory init failed!");

  if (!S9xInitAPU())
    RG_PANIC("APU init failed!");

  if (!S9xInitSound(0, 0))
    RG_PANIC("Sound init failed!");

  if (!S9xInitGFX())
    RG_PANIC("Graphics init failed!");

  const char *filename = app->romPath;

  if (rg_extension_match(filename, "zip")) {
    if (!rg_storage_unzip_file(filename, NULL, (void **)&Memory.ROM,
                               &Memory.ROM_AllocSize, RG_FILE_USER_BUFFER))
      RG_PANIC("ROM file unzipping failed!");
    filename = NULL;
  }

  if (!LoadROM(filename))
    RG_PANIC("ROM loading failed!");

  load_sram();

#ifdef USE_BLARGG_APU
  S9xSetSamplesAvailableCallback(S9xAudioCallback);
#else
  S9xSetPlaybackRate(Settings.SoundPlaybackRate);
#endif

  if (app->bootFlags & RG_BOOT_RESUME) {
    rg_emu_load_state(app->saveSlot);
  }

  rg_system_set_tick_rate(Memory.ROMFramesPerSecond);
  app->frameskip = 0;

  bool menuCancelled = false;
  bool menuPressed = false;
  bool slowFrame = false;
  int skipFrames = 0;

  int last_overclock = rg_system_get_overclock();
  int oc_poll_counter = 0;

  while (1) {
    uint32_t joystick = rg_input_read_gamepad();

    // Poll overclock changes at ~1Hz (every 60 frames) to reduce per-frame overhead.
    if (++oc_poll_counter >= 60) {
      oc_poll_counter = 0;
      int current_overclock = rg_system_get_overclock();
      if (current_overclock != last_overclock) {
        last_overclock = current_overclock;
        save_config();
      }
    }

    if (CPU.SRAMModified) {
      save_sram(false);
    }

    if (menuPressed && !(joystick & RG_KEY_MENU)) {
      if (!menuCancelled) {
        rg_task_delay(50);
        rg_gui_game_menu();
      }
      menuCancelled = false;
    }
    if (joystick & RG_KEY_OPTION) {
      rg_gui_options_menu();
      rg_display_submit(NULL, 0); // Force UI refresh and scaling update
    }

    menuPressed = joystick & RG_KEY_MENU;

    if (menuPressed && joystick & ~RG_KEY_MENU) {
      menuCancelled = true;
    }

    int64_t startTime = rg_system_timer();
    bool drawFrame = (skipFrames == 0);

    if (drawFrame) {
      currentBufferIndex = (currentBufferIndex + 1) % 3;
      currentUpdate = updates[currentBufferIndex];
      GFX.Screen = currentUpdate->data;
    }

    IPPU.RenderThisFrame = drawFrame;

    S9xMainLoop();

    if (drawFrame) {
      slowFrame = !rg_display_sync(false);
      currentUpdate->width = (uint16_t)IPPU.RenderedScreenWidth;
      currentUpdate->height = (uint16_t)IPPU.RenderedScreenHeight;
      rg_display_submit(currentUpdate, RG_DISPLAY_WRITE_NOSYNC);
    }

#ifndef USE_BLARGG_APU
    // Audio must be mixed every frame regardless of frame-skip.
    // The SNES APU runs independently and continuous draining prevents buffer underruns.
    if (apu_enabled && lowpass_filter)
      S9xMixSamplesLowPass((void *)audioBuffer, AUDIO_BUFFER_LENGTH << 1,
                           AUDIO_LOW_PASS_RANGE);
    else if (apu_enabled)
      S9xMixSamples((void *)audioBuffer, AUDIO_BUFFER_LENGTH << 1);

    if (apu_enabled) {
      // Optimized audio boost using saturation logic
      for (int i = 0; i < AUDIO_BUFFER_LENGTH; i++) {
        int32_t left = (int32_t)audioBuffer[i].left << 1;
        int32_t right = (int32_t)audioBuffer[i].right << 1;
#ifdef __XTENSA__
        __asm__ __volatile__("clamps %0, %1, 15" : "=r"(left) : "r"(left));
        __asm__ __volatile__("clamps %0, %1, 15" : "=r"(right) : "r"(right));
#else
        if (left > 32767) left = 32767;
        else if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        else if (right < -32768) right = -32768;
#endif
        audioBuffer[i].left = (int16_t)left;
        audioBuffer[i].right = (int16_t)right;
      }
      rg_audio_submit(audioBuffer, AUDIO_BUFFER_LENGTH);
    }
#endif

    rg_system_tick(rg_system_timer() - startTime);

    if (skipFrames == 0) {
      int elapsed = rg_system_timer() - startTime;
      if (app->frameskip > 0)
        skipFrames = app->frameskip;
      else if (elapsed > app->frameTime + 1500) // Allow some jitter
        skipFrames = 1;                         // (elapsed / frameTime)
      else if (drawFrame && slowFrame)
        skipFrames = 1;
    } else if (skipFrames > 0) {
      skipFrames--;
    }
  }
}
