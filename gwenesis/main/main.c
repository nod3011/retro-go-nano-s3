#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gwenesis.h>

#define AUDIO_SAMPLE_RATE (44100)
#define AUDIO_BUFFER_LENGTH (1024) // Enough for 44.1kHz at 50Hz (882) plus jitter

extern unsigned char *VRAM;
extern int zclk;
int system_clock;
int scan_line;

// Audio buffers MUST be in internal DRAM (not PSRAM) because:
// 1. I2S DMA accesses them directly; if in PSRAM, the SPI clock divider at OC3+
//    (/8 instead of /7) causes DMA stalls and complete audio silence.
// 2. DRAM_ATTR forces placement in .dram0.data (fast internal SRAM, writable).
//    NOTE: Do NOT use IRAM_ATTR here — that targets .iram0.text (instruction RAM),
//    which is read-only on ESP32-S3 and would cause a CPU exception on writes.
// Audio buffers in internal DRAM
DRAM_ATTR int16_t gwenesis_sn76489_buffer[AUDIO_BUFFER_LENGTH];
int sn76489_index;
int sn76489_clock;
DRAM_ATTR int16_t gwenesis_ym2612_buffer[AUDIO_BUFFER_LENGTH];
int ym2612_index;
int ym2612_clock;
static DRAM_ATTR rg_audio_frame_t gwenesis_mix_buffer[AUDIO_BUFFER_LENGTH];

static FILE *savestate_fp = NULL;
static int savestate_errors = 0;

static bool yfm_enabled = true;
static bool z80_enabled = true;
static bool sn76489_enabled = true;

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_app_t *app;

static const char *SETTING_YFM_EMULATION = "yfm_enable";
static const char *SETTING_Z80_EMULATION = "z80_enable";
static const char *SETTING_SN76489_EMULATION = "sn_enable";

static int btn_a_map = 1;     // Default: B
static int btn_b_map = 0;     // Default: A
static int btn_c_map = 2;     // Default: Select
static int btn_start_map = 3; // Default: Start

static const char *btn_names[] = {"A", "B", "Select", "Start"};
static const uint32_t btn_keys[] = {RG_KEY_A, RG_KEY_B, RG_KEY_SELECT,
                                    RG_KEY_START};

static uint32_t keymap[8] = {RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT,
                             RG_KEY_B,  RG_KEY_A,    RG_KEY_SELECT, RG_KEY_START};

static struct {
  int *val;
  const char *key;
  int index;
} btn_configs[4] = {
    {&btn_a_map, "btn_a", 4},
    {&btn_b_map, "btn_b", 5},
    {&btn_c_map, "btn_c", 6},
    {&btn_start_map, "btn_start", 7},
};

static bool turbo_a_toggled = false;
static bool turbo_b_toggled = false;
static bool menu_cancelled = false;
static int turbo_counter = 0;

IRAM_ATTR static void gwenesis_audio_mix_and_submit(size_t count) {
  if (count == 0) return;
  if (count > AUDIO_BUFFER_LENGTH) count = AUDIO_BUFFER_LENGTH;
  
  if (yfm_enabled && sn76489_enabled) {
    for (size_t i = 0; i < count; i++) {
      int32_t mono = 0;
      if (i < ym2612_index) mono += gwenesis_ym2612_buffer[i];
      if (i < sn76489_index) mono += gwenesis_sn76489_buffer[i];
      if (mono > 32767) mono = 32767; 
      else if (mono < -32768) mono = -32768;
      gwenesis_mix_buffer[i].left = (int16_t)mono;
      gwenesis_mix_buffer[i].right = (int16_t)mono;
    }
  } else if (yfm_enabled) {
    for (size_t i = 0; i < count; i++) {
      int16_t mono = (i < ym2612_index) ? gwenesis_ym2612_buffer[i] : 0;
      gwenesis_mix_buffer[i].left = mono;
      gwenesis_mix_buffer[i].right = mono;
    }
  } else if (sn76489_enabled) {
    for (size_t i = 0; i < count; i++) {
      int16_t mono = (i < sn76489_index) ? gwenesis_sn76489_buffer[i] : 0;
      gwenesis_mix_buffer[i].left = mono;
      gwenesis_mix_buffer[i].right = mono;
    }
  } else {
    memset(gwenesis_mix_buffer, 0, count * sizeof(rg_audio_frame_t));
  }
  
  rg_audio_submit(gwenesis_mix_buffer, count);
}

static void load_config();
static void save_config();

// --- MAIN

typedef struct {
  char key[28];
  uint32_t length;
} svar_t;

SaveState *saveGwenesisStateOpenForRead(const char *fileName) {
  return (void *)1;
}

SaveState *saveGwenesisStateOpenForWrite(const char *fileName) {
  return (void *)1;
}

int saveGwenesisStateGet(SaveState *state, const char *tagName) {
  int value = 0;
  saveGwenesisStateGetBuffer(state, tagName, &value, sizeof(int));
  return value;
}

void saveGwenesisStateSet(SaveState *state, const char *tagName, int value) {
  saveGwenesisStateSetBuffer(state, tagName, &value, sizeof(int));
}

void saveGwenesisStateGetBuffer(SaveState *state, const char *tagName,
                                void *buffer, int length) {
  size_t initial_pos = ftell(savestate_fp);
  bool from_start = false;
  svar_t var;

  // Odds are that calls to this func will be in order, so try searching from
  // current file position.
  while (!from_start || ftell(savestate_fp) < initial_pos) {
    if (!fread(&var, sizeof(svar_t), 1, savestate_fp)) {
      if (!from_start) {
        fseek(savestate_fp, 0, SEEK_SET);
        from_start = true;
        continue;
      }
      break;
    }
    if (strncmp(var.key, tagName, sizeof(var.key)) == 0) {
      fread(buffer, RG_MIN(var.length, length), 1, savestate_fp);
      RG_LOGI("Loaded key '%s'\n", tagName);
      return;
    }
    fseek(savestate_fp, var.length, SEEK_CUR);
  }
  RG_LOGW("Key %s NOT FOUND!\n", tagName);
  savestate_errors++;
}

void saveGwenesisStateSetBuffer(SaveState *state, const char *tagName,
                                void *buffer, int length) {
  // TO DO: seek the file to find if the key already exists. It's possible it
  // could be written twice.
  svar_t var = {{0}, length};
  strncpy(var.key, tagName, sizeof(var.key) - 1);
  fwrite(&var, sizeof(var), 1, savestate_fp);
  fwrite(buffer, length, 1, savestate_fp);
  RG_LOGI("Saved key '%s'\n", tagName);
}


static rg_gui_event_t yfm_update_cb(rg_gui_option_t *option,
                                    rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    yfm_enabled = !yfm_enabled;
    rg_settings_set_number(NS_APP, SETTING_YFM_EMULATION, yfm_enabled);
  }
  strcpy(option->value, yfm_enabled ? _("On") : _("Off"));

  return RG_DIALOG_VOID;
}

static rg_gui_event_t sn76489_update_cb(rg_gui_option_t *option,
                                        rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    sn76489_enabled = !sn76489_enabled;
    rg_settings_set_number(NS_APP, SETTING_SN76489_EMULATION, sn76489_enabled);
  }
  strcpy(option->value, sn76489_enabled ? _("On") : _("Off"));

  return RG_DIALOG_VOID;
}

static rg_gui_event_t z80_update_cb(rg_gui_option_t *option,
                                    rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    z80_enabled = !z80_enabled;
    rg_settings_set_number(NS_APP, SETTING_Z80_EMULATION, z80_enabled);
    if (!z80_enabled) {
      zclk = 0x1000000;
    } else {
      zclk = 0;
    }
  }
  strcpy(option->value, z80_enabled ? _("On") : _("Off"));

  return RG_DIALOG_VOID;
}

static rg_gui_event_t sub_btn_mapping_cb(rg_gui_option_t *option,
                                         rg_gui_event_t event) {
  int i = (int)option->arg;
  int *val = btn_configs[i].val;
  int index = btn_configs[i].index;

  if (event == RG_DIALOG_PREV)
    *val = (*val + 3) % 4;
  if (event == RG_DIALOG_NEXT)
    *val = (*val + 1) % 4;

  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    keymap[index] = btn_keys[*val];
  }

  strcpy(option->value, btn_names[*val]);

  return RG_DIALOG_VOID;
}

static rg_gui_event_t save_config_cb(rg_gui_option_t *option,
                                     rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    save_config();
    rg_gui_alert(_("Success"), _("Configuration saved."));
  }
  return RG_DIALOG_VOID;
}

static rg_gui_event_t load_config_cb(rg_gui_option_t *option,
                                     rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    load_config();
    rg_gui_alert(_("Success"), _("Configuration loaded."));
    return RG_DIALOG_REDRAW;
  }
  return RG_DIALOG_VOID;
}

static rg_gui_event_t btn_mapping_cb(rg_gui_option_t *option,
                                     rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    rg_gui_option_t options[7];
    options[0] = (rg_gui_option_t){0, _("Button A"), "-", RG_DIALOG_FLAG_NORMAL,
                                   &sub_btn_mapping_cb};
    options[1] = (rg_gui_option_t){1, _("Button B"), "-", RG_DIALOG_FLAG_NORMAL,
                                   &sub_btn_mapping_cb};
    options[2] = (rg_gui_option_t){2, _("Button C"), "-", RG_DIALOG_FLAG_NORMAL,
                                   &sub_btn_mapping_cb};
    options[3] = (rg_gui_option_t){3, _("Button Start"), "-",
                                   RG_DIALOG_FLAG_NORMAL, &sub_btn_mapping_cb};
    options[4] = (rg_gui_option_t){0, _("Save Config"), NULL,
                                   RG_DIALOG_FLAG_NORMAL, &save_config_cb};
    options[5] = (rg_gui_option_t){0, _("Load Config"), NULL,
                                   RG_DIALOG_FLAG_NORMAL, &load_config_cb};
    options[6] = (rg_gui_option_t)RG_DIALOG_END;

    rg_gui_dialog(option->label, options, 0);
  }
  return RG_DIALOG_VOID;
}

static bool screenshot_handler(const char *filename, int width, int height) {
  return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename) {
  if ((savestate_fp = fopen(filename, "wb"))) {
    savestate_errors = 0;
    gwenesis_save_state();
    fclose(savestate_fp);
    return savestate_errors == 0;
  }
  return false;
}

static bool load_state_handler(const char *filename) {
  if ((savestate_fp = fopen(filename, "rb"))) {
    savestate_errors = 0;
    gwenesis_load_state();
    fclose(savestate_fp);
    if (savestate_errors == 0)
      return true;
  }
  reset_emulation();
  return false;
}

static bool reset_handler(bool hard) {
  reset_emulation();
  return true;
}

static void event_handler(int event, void *arg) {
  if (event == RG_EVENT_REDRAW) {
    rg_display_submit(currentUpdate, 0);
  }
}

static void options_handler(rg_gui_option_t *dest) {
  *dest++ = (rg_gui_option_t){0, _("YM2612 audio "), "-", RG_DIALOG_FLAG_NORMAL,
                              &yfm_update_cb};
  *dest++ = (rg_gui_option_t){0, _("SN76489 audio"), "-", RG_DIALOG_FLAG_NORMAL,
                              &sn76489_update_cb};
  *dest++ = (rg_gui_option_t){0, _("Z80 emulation"), "-", RG_DIALOG_FLAG_NORMAL,
                              &z80_update_cb};

  *dest++ = (rg_gui_option_t){0, _("Map Buttons"), "...", RG_DIALOG_FLAG_NORMAL,
                              &btn_mapping_cb};
  *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

static void load_config() {
  char path[RG_PATH_MAX];
  snprintf(path, sizeof(path), "%s/md/%s.cfg", RG_BASE_PATH_CONFIG,
           rg_basename(app->romPath));

  void *data = NULL;
  size_t size = 0;
  if (rg_storage_read_file(path, &data, &size, 0)) {
    if (size >= sizeof(int) * 4) {
      int *vals = (int *)data;
      btn_a_map = vals[0];
      btn_b_map = vals[1];
      btn_c_map = vals[2];
      btn_start_map = vals[3];
      RG_LOGI("Config loaded from %s\n", path);
    }
    free(data);
  }

  keymap[4] = btn_keys[btn_a_map];
  keymap[5] = btn_keys[btn_b_map];
  keymap[6] = btn_keys[btn_c_map];
  keymap[7] = btn_keys[btn_start_map];
}

static void save_config() {
  char path[RG_PATH_MAX];
  snprintf(path, sizeof(path), "%s/md/%s.cfg", RG_BASE_PATH_CONFIG,
           rg_basename(app->romPath));
           
  rg_storage_mkdir(rg_dirname(path));

  int vals[4] = {btn_a_map, btn_b_map, btn_c_map, btn_start_map};
  if (rg_storage_write_file(path, vals, sizeof(vals), 0)) {
    RG_LOGI("Config saved to %s\n", path);
  }
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

  app = rg_system_init(AUDIO_SAMPLE_RATE, &handlers, NULL);
  app->screenSync = 1;
  // Only set default overclock=2 if the user hasn't saved a per-ROM preference.
  // NS_FILE overclock was already loaded by rg_system_init — don't override it.
  if (!rg_settings_exists(NS_FILE, "overclock")) {
    rg_system_set_overclock(2);
  }

  yfm_enabled = rg_settings_get_number(NS_APP, SETTING_YFM_EMULATION, 1);
  sn76489_enabled = rg_settings_get_number(NS_APP, SETTING_SN76489_EMULATION, 1);
  z80_enabled = rg_settings_get_number(NS_APP, SETTING_Z80_EMULATION, 1);

  load_config();

  updates[0] = rg_surface_create(320, 241, RG_PIXEL_PAL565_BE, MEM_FAST);
  // updates[1] = rg_surface_create(320, 241, RG_PIXEL_PAL565_BE, MEM_FAST);
  currentUpdate = updates[0];

  // This is a hack because our new surface format doesn't yet support overdraw
  // space easily
  updates[0]->data += 160;
  updates[0]->height = 240;
  // updates[1]->data += 160;
  // updates[1]->height = 240;


  RG_LOGI("Genesis start\n");

  size_t rom_size;
  void *rom_data;

  if (rg_extension_match(app->romPath, "zip")) {
    if (!rg_storage_unzip_file(app->romPath, NULL, &rom_data, &rom_size,
                               RG_FILE_ALIGN_64KB))
      RG_PANIC("ROM file unzipping failed!");
  } else if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size,
                                   RG_FILE_ALIGN_64KB)) {
    RG_PANIC("ROM load failed!");
  }

  RG_LOGI("load_cartridge(%p, %zu)\n", rom_data, rom_size);
  // In RETRO_GO mode, ROM_DATA = buffer (takes direct ownership, do NOT free).
  // If not RETRO_GO, load_cartridge does memcpy internally so buffer can be freed after.
  load_cartridge(rom_data, rom_size);
  // rom_data is now owned by ROM_DATA pointer in gwenesis_bus.c — do not free!

  RG_LOGI("power_on()\n");
  power_on();

  RG_LOGI("reset_emulation()\n");
  reset_emulation();

  if (app->bootFlags & RG_BOOT_RESUME) {
    rg_emu_load_state(app->saveSlot);
  }

  rg_system_set_tick_rate(60);
  app->frameskip = 0;

  extern unsigned char gwenesis_vdp_regs[0x20];
  extern unsigned short gwenesis_vdp_status;
  extern unsigned short *CRAM565;
  extern int screen_width, screen_height;
  extern int hint_pending;
  extern int zclk;
  extern bool gwenesis_cram_dirty;

  zclk = z80_enabled ? 0 : 0x1000000;

  // index: 0=Up, 1=Down, 2=Left, 3=Right, 4=Gen_A, 5=Gen_B, 6=Gen_C,
  // 7=Gen_Start
  uint32_t joystick = 0, joystick_old = 0, effective_old = 0;

  int skipFrames = 0;

  RG_LOGI("emulation loop\n");
  while (!rg_system_exit_called()) {
    joystick_old = joystick;
    joystick = rg_input_read_gamepad();
    uint32_t joystick_down = joystick & ~joystick_old;
    turbo_counter++;

    if (joystick & RG_KEY_MENU) {
      if (joystick_down & RG_KEY_A) {
        turbo_a_toggled = !turbo_a_toggled;
        RG_LOGI("Turbo A: %s\n", turbo_a_toggled ? "ON" : "OFF");
      }
      if (joystick_down & RG_KEY_B) {
        turbo_b_toggled = !turbo_b_toggled;
        RG_LOGI("Turbo B: %s\n", turbo_b_toggled ? "ON" : "OFF");
      }
      if (joystick & ~RG_KEY_MENU) {
        menu_cancelled = true;
      }
    } else {
      if (joystick_old & RG_KEY_MENU) {
        if (!menu_cancelled)
          rg_gui_game_menu();
        menu_cancelled = false;
      }
    }

    if (joystick & RG_KEY_OPTION) {
      rg_gui_options_menu();
    }

    uint32_t effective = joystick;
    if (joystick & RG_KEY_MENU) {
      effective = 0; // Don't pass inputs if menu is held (as modifier)
    } else {
      if (turbo_a_toggled && (joystick & RG_KEY_A) && (turbo_counter & 4))
        effective &= ~RG_KEY_A;
      if (turbo_b_toggled && (joystick & RG_KEY_B) && (turbo_counter & 4))
        effective &= ~RG_KEY_B;
    }

    if (effective != effective_old) {
      for (int i = 0; i < 8; i++) {
        uint32_t key = keymap[i];
        if ((effective & key) == key)
          gwenesis_io_pad_press_button(0, i);
        else
          gwenesis_io_pad_release_button(0, i);
      }
      effective_old = effective;
    }

    int64_t startTime = rg_system_timer();
    bool drawFrame = (skipFrames == 0);

    int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
    int hint_counter = gwenesis_vdp_regs[10];

    screen_width = REG12_MODE_H40 ? 320 : 256;
    screen_height = REG1_PAL ? 240 : 224;

    gwenesis_vdp_set_buffer(currentUpdate->data);
    gwenesis_vdp_render_config();

    /* Reset the difference clocks and audio index */
    system_clock = 0;

    ym2612_clock = yfm_enabled ? 0 : 0x1000000;
    ym2612_index = 0;

    sn76489_clock = sn76489_enabled ? 0 : 0x1000000;
    sn76489_index = 0;

    scan_line = 0;

    while (scan_line < lines_per_frame) {
      m68k_run(system_clock + VDP_CYCLES_PER_LINE);
      z80_run(system_clock + VDP_CYCLES_PER_LINE);

      /* Audio */
      /*  GWENESIS_AUDIO_ACCURATE:
       *    =1 : cycle accurate mode. audio is refreshed when CPUs are
       * performing a R/W access =0 : line  accurate mode. audio is refreshed
       * every lines.
       */
      if (GWENESIS_AUDIO_ACCURATE == 0) {
        gwenesis_SN76489_run(system_clock + VDP_CYCLES_PER_LINE);
        ym2612_run(system_clock + VDP_CYCLES_PER_LINE);
      }

      /* Video */
      if (drawFrame && scan_line < screen_height)
        gwenesis_vdp_render_line(scan_line); /* render scan_line */

      // On these lines, the line counter interrupt is reloaded
      if ((scan_line == 0) || (scan_line > screen_height)) {
        //  if (REG0_LINE_INTERRUPT != 0)
        //    printf("HINTERRUPT counter reloaded: (scan_line: %d, new
        //    counter: %d)\n", scan_line, REG10_LINE_COUNTER);
        hint_counter = REG10_LINE_COUNTER;
      }

      // interrupt line counter
      if (--hint_counter < 0) {
        if ((REG0_LINE_INTERRUPT != 0) && (scan_line <= screen_height)) {
          hint_pending = 1;
          // printf("Line int pending %d\n",scan_line);
          if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
            m68k_update_irq(4);
        }
        hint_counter = REG10_LINE_COUNTER;
      }

      scan_line++;

      // vblank begin at the end of last rendered line
      if (scan_line == screen_height) {
        if (REG1_VBLANK_INTERRUPT != 0) {
          gwenesis_vdp_status |= STATUS_VIRQPENDING;
          m68k_set_irq(6);
        }
        z80_irq_line(1);
      }
      if (scan_line == (screen_height + 1)) {
        z80_irq_line(0);
      }

      system_clock += VDP_CYCLES_PER_LINE;
    }

    /* Audio
     * synchronize YM2612 and SN76489 to system_clock
     * it completes the missing audio sample for accurate audio mode
     */
    if (GWENESIS_AUDIO_ACCURATE == 1) {
      gwenesis_SN76489_run(system_clock);
      ym2612_run(system_clock);
    }

    // reset m68k & z80 cycles to the begin of next frame cycle
    m68k.cycles -= system_clock;
    if (z80_enabled) zclk -= system_clock;

    if (drawFrame) {
      if (gwenesis_cram_dirty) {
        for (int i = 0; i < 64; ++i)
          currentUpdate->palette[i] = (CRAM565[i] << 8) | (CRAM565[i] >> 8);
        memcpy(&currentUpdate->palette[64],  &currentUpdate->palette[0], 64 * sizeof(uint16_t));
        memcpy(&currentUpdate->palette[128], &currentUpdate->palette[0], 64 * sizeof(uint16_t));
        memcpy(&currentUpdate->palette[192], &currentUpdate->palette[0], 64 * sizeof(uint16_t));
        gwenesis_cram_dirty = false;
      }
      currentUpdate->width = screen_width;
      currentUpdate->height = screen_height;
      rg_display_submit(currentUpdate, 0); 
    }

    // Audio-Clock Sync: Let audio hardware drive the emulator speed
    if (yfm_enabled || sn76489_enabled) {
      size_t count = (ym2612_index > sn76489_index) ? ym2612_index : sn76489_index;
      if (count > AUDIO_BUFFER_LENGTH) count = AUDIO_BUFFER_LENGTH;
      gwenesis_audio_mix_and_submit(count); 
    }

    // Capture Busy Time for Frameskip indicator/logic
    int64_t busyTime = rg_system_timer() - startTime;
    rg_system_tick(busyTime);

    // --- Robust Frameskip Manager ---
    static int consecutive_skips = 0;

    if (app->frameskip > 0) {
      // Manual Frameskip: simple toggle based on counter
      if (skipFrames > 0) skipFrames--;
      else skipFrames = app->frameskip;
    } else {
      // Auto Frameskip: skip if busyTime > frameTime + 1ms slack
      // But NEVER skip more than 3 frames in a row to prevent freezing.
      if (busyTime > (app->frameTime + 1000) && consecutive_skips < 3) {
        skipFrames = 1;
        consecutive_skips++;
      } else {
        skipFrames = 0;
        consecutive_skips = 0;
      }
    }
    // --------------------------------
  }

  RG_LOGI("Genesis ended");

  if (rom_data)
    free(rom_data);

  gwenesis_vdp_free();
  gwenesis_bus_free();

  rg_surface_free(updates[0]);
  if (updates[1])
    rg_surface_free(updates[1]);

  rg_system_exit();
}
