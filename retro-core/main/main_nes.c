#include "cheat.h"
#include "fceu.h"
#include "nes_palettes.h"
#include "rom_manager.h"
#include "shared.h"
#include <fceumm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdio.h>
#include <stdlib.h>
#include <streams/memory_stream.h>
#include <string.h>


static const char *SETTING_PALETTE = "palette";

#define NES_WIDTH 256
#define NES_HEIGHT 240

// --- GLOBALS
static rg_app_t *app;
static rg_surface_t *updates[3];
static rg_surface_t *currentUpdate;
static int currentBufferIndex = 0;
static int8_t current_frameskip = -1; // Default to Auto

#define AUDIO_BUFFER_COUNT 3
#define AUDIO_BUFFER_SIZE 1024

typedef struct {
  rg_audio_frame_t frames[AUDIO_BUFFER_SIZE];
  size_t count;
} audio_msg_t;

// Audio buffers MUST be in internal DRAM (not PSRAM) for stable I2S DMA on S3
static DRAM_ATTR audio_msg_t audio_pool[AUDIO_BUFFER_COUNT];
static QueueHandle_t audio_queue_full;
static QueueHandle_t audio_queue_empty;

static void sync_audio_to_system() {
  double freq = rg_system_get_cpu_speed();
  if (freq < 100)
    freq = 240.0;

  // The "Universal S3 Sync Formula":
  // Adjust hardware rate to compensate for S3 APB/I2S clock drift during OC.
  // The +0.4 offset accounts for fractional divider rounding.
  double target_rate = (double)AUDIO_SAMPLE_RATE * (240.0 / (freq + 0.4));

  rg_audio_set_sample_rate((int)target_rate);
}

static void audio_task(void *arg) {
  audio_msg_t *msg;
  while (!rg_system_exit_called()) {
    if (xQueueReceive(audio_queue_full, &msg, pdMS_TO_TICKS(100))) {
      rg_audio_submit(msg->frames, msg->count);
      xQueueSend(audio_queue_empty, &msg, portMAX_DELAY);
    }
  }
  vTaskDelete(NULL);
}

#ifndef RG_ATTR_EXT_RAM
#define RG_ATTR_EXT_RAM __attribute__((section(".ext_ram.bss")))
#endif

typedef struct {
  uint32_t magic;
  int overclock;
  int8_t frameskip;
  int palette;
} nes_config_t;

static uint16_t palette565[256];
static int palette_dirty = 0;
static uint32_t fceu_joystick;

void GetKeyboard(void) {}

// Forward declarations for memstream (from libretro-common)
extern void memstream_set_buffer(uint8_t *buffer, uint64_t size);
extern uint64_t memstream_get_last_size(void);

// Implementation of FCEUSS_Save_Fs and FCEUSS_Load_Fs using memstream
bool FCEUSS_Save_Fs(const char *path) {
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

bool FCEUSS_Load_Fs(const char *path) {
  void *buffer = NULL;
  size_t size = 0;
  if (!rg_storage_read_file(path, &buffer, &size, 0))
    return false;

  memstream_set_buffer((uint8_t *)buffer, size);
  FCEUSS_Load_Mem();
  free(buffer);
  return true;
}

CartInfo *get_cart_info(void) {
  if (iNESCart.mapper >= 0 || iNESCart.PRGRomSize > 0)
    return &iNESCart;
  if (UNIFCart.PRGRomSize > 0)
    return &UNIFCart;
  return NULL;
}

static bool load_sram(void) {
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
  if (!path)
    return false;

  void *buffer = NULL;
  size_t size = 0;
  if (!rg_storage_read_file(path, &buffer, &size, 0)) {
    free(path);
    return false;
  }

  CartInfo *cart = get_cart_info();
  if (!cart) {
    free(path);
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

  free(path);
  free(buffer);
  return loaded;
}

static bool save_sram(void) {
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
  CartInfo *cart = get_cart_info();
  if (!path)
    return false;
  if (!cart) {
    free(path);
    return false;
  }

  for (int i = 0; i < 4; i++) {
    if (cart->SaveGame[i] && cart->SaveGameLen[i] > 0) {
      bool ret = rg_storage_write_file(path, cart->SaveGame[i],
                                       cart->SaveGameLen[i], 0);
      free(path);
      return ret;
    }
  }
  free(path);
  return false;
}

static void save_config() {
  char path[RG_PATH_MAX];
  snprintf(path, sizeof(path), "%s/nes/%s.cfg", RG_BASE_PATH_CONFIG,
           rg_basename(app->romPath));

  rg_storage_mkdir(rg_dirname(path));

  nes_config_t cfg = {
      .magic = 0x46434555, // "FCEU"
      .overclock = rg_system_get_overclock(),
      .frameskip = current_frameskip,
      .palette = rg_settings_get_number(app->configNs, SETTING_PALETTE, 0),
  };

  if (rg_storage_write_file(path, &cfg, sizeof(cfg), 0)) {
    RG_LOGI("Config saved to %s (OC:%d, FS:%d, PAL:%d)\n", path, cfg.overclock,
            cfg.frameskip, cfg.palette);
  }
}

static void load_config() {
  char path[RG_PATH_MAX];
  snprintf(path, sizeof(path), "%s/nes/%s.cfg", RG_BASE_PATH_CONFIG,
           rg_basename(app->romPath));

  void *data = NULL;
  size_t size = 0;
  if (rg_storage_read_file(path, &data, &size, 0)) {
    if (size >= sizeof(nes_config_t)) {
      nes_config_t *cfg = (nes_config_t *)data;
      if (cfg->magic == 0x46434555) {
        if (cfg->overclock >= 0 && cfg->overclock <= 3) {
          rg_system_set_overclock(cfg->overclock);
        }
        current_frameskip = cfg->frameskip;
        rg_settings_set_number(app->configNs, SETTING_PALETTE, cfg->palette);
        RG_LOGI("Config loaded from %s (OC:%d, FS:%d, PAL:%d)\n", path,
                rg_system_get_overclock(), current_frameskip, cfg->palette);
      }
    }
    free(data);
  }
}

static void update_audio(int32_t *samples, size_t count) {
  if (count <= 0 || !samples)
    return;

  audio_msg_t *msg = NULL;
  // Get an empty buffer (Blocks here if emulator is > 1 frame ahead)
  if (xQueueReceive(audio_queue_empty, &msg, portMAX_DELAY) != pdTRUE)
    return;

  if (count > AUDIO_BUFFER_SIZE)
    count = AUDIO_BUFFER_SIZE;

  for (size_t i = 0; i < count; i++) {
    int32_t s = samples[i];
    if (s > 32767)
      s = 32767;
    else if (s < -32768)
      s = -32768;
    msg->frames[i].left = (int16_t)s;
    msg->frames[i].right = (int16_t)s;
  }
  msg->count = count;

  // Send to playback (Blocking here ensures strict 100% speed)
  xQueueSend(audio_queue_full, &msg, portMAX_DELAY);
}

// --- FCEU CALLBACKS
void FCEUD_PrintError(char *c) { RG_LOGE("%s\n", c); }
void FCEUD_Message(char *s) { /* Noise */ }
void FCEUD_DispMessage(enum retro_log_level level, unsigned duration,
                       const char *str) {
  /* Noise */
}

void FCEUD_SetPalette(uint8 index, uint8 r, uint8 g, uint8 b) {
  // Store as 565 Big-Endian for hardware optimized blitting
  uint16_t color = (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  palette565[index] = (color >> 8) | (color << 8);
  palette_dirty = 3; // Ensure all 3 triple-buffers get the update
}

// Game Genie Stubs
void FCEU_OpenGenie(void) {}
void FCEU_CloseGenie(void) {}
void FCEU_GeniePower(void) {}

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

// --- CHEATS
static void apply_cheat_code(const char *code, const char *name, int status) {
  if (!code || strlen(code) < 6)
    return;

  uint16 a_16;
  uint8 v;
  int comp;

  if (!FCEUI_DecodeGG(code, &a_16, &v, &comp)) {
    RG_LOGE("Invalid Game Genie code: %s\n", code);
    return;
  }

  uint32 a = a_16;

  // Use description format: "NAME|CODE"
  char full_desc[128];
  snprintf(full_desc, sizeof(full_desc), "%s|%s", name ? name : "Cheat", code);
  FCEUI_AddCheat(full_desc, a, v, comp,
                 1); // Always use type 1 (Sub) for Game Genie

  // Set initial status
  // We identify the cheat by its index, which is the last one added.
  int total = 0;
  while (FCEUI_GetCheat(total, NULL, NULL, NULL, NULL, NULL, NULL))
    total++;

  if (total > 0) {
    FCEUI_SetCheat(total - 1, NULL, -1, -1, -1, status, 1);
  }
}

static void load_cheats(void) {
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
  if (!path)
    return;

  char *saves_str = strstr(path, "saves");
  if (saves_str) {
    memcpy(saves_str, "cheat", 5);
  }

  char *ext = strrchr(path, '.');
  if (ext)
    strcpy(ext, ".cht");

  void *buffer = NULL;
  size_t size = 0;
  if (!rg_storage_read_file(path, &buffer, &size, 0)) {
    free(path);
    return;
  }

  FCEU_ResetCheats();

  char *line = strtok((char *)buffer, "\r\n");
  while (line) {
    char *sep1 = strchr(line, '|');
    if (sep1) {
      *sep1 = 0;
      char *name = line;
      char *code_part = sep1 + 1;
      int status = 1; // Default to ON

      char *sep2 = strchr(code_part, '|');
      if (sep2) {
        *sep2 = 0;
        char *status_str = sep2 + 1;
        if (strcmp(status_str, "OFF") == 0)
          status = 0;
      }
      apply_cheat_code(code_part, name, status);
    }
    line = strtok(NULL, "\r\n");
  }

  free(buffer);
  free(path);
}

static void save_cheats(void) {
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
  if (!path)
    return;

  char *saves_str = strstr(path, "saves");
  if (saves_str) {
    memcpy(saves_str, "cheat", 5);
  }

  rg_storage_mkdir(rg_dirname(path));

  char *ext = strrchr(path, '.');
  if (ext)
    strcpy(ext, ".cht");

  const size_t buffer_size = 16384; // 16KB should be plenty
  char *buffer = malloc(buffer_size);
  if (!buffer) {
    free(path);
    return;
  }
  buffer[0] = 0;
  size_t offset = 0;

  for (int i = 0; i < 64; i++) {
    uint32 a;
    uint8 v;
    int s, t, comp;
    char *full_name = NULL;
    if (!FCEUI_GetCheat(i, &full_name, &a, &v, &comp, &s, &t))
      break;

    if (full_name) {
      int len = snprintf(buffer + offset, buffer_size - offset, "%s|%s\n",
                         full_name, s ? "ON" : "OFF");
      if (len > 0 && offset + len < buffer_size) {
        offset += len;
      } else {
        RG_LOGW("Cheat buffer full, some cheats might not be saved!\n");
        break;
      }
    }
  }

  if (offset > 0) {
    rg_storage_write_file(path, buffer, offset, 0);
  } else {
    rg_storage_delete(path);
  }

  free(buffer);
  free(path);
}

static int last_cheat_sel = 0;
static rg_gui_event_t cheat_toggle_cb(rg_gui_option_t *opt,
                                      rg_gui_event_t event) {
  if (!opt)
    return RG_DIALOG_VOID;

  int index = (int)opt->arg;
  uint32 a;
  uint8 v;
  int s, t, comp;
  char *name = NULL;

  if (event == RG_DIALOG_INIT || event == RG_DIALOG_UPDATE) {
    if (opt->value && FCEUI_GetCheat(index, &name, &a, &v, &comp, &s, &t)) {
      strcpy(opt->value, s ? "ON" : "OFF");
    }
    return RG_DIALOG_VOID;
  }

  if (event != RG_DIALOG_ENTER && event != RG_DIALOG_SELECT)
    return RG_DIALOG_VOID;

  if (FCEUI_GetCheat(index, &name, &a, &v, &comp, &s, &t)) {
    FCEUI_SetCheat(index, NULL, -1, -1, -1, !s, t);
    save_cheats();
    return RG_DIALOG_UPDATE;
  }
  return RG_DIALOG_VOID;
}

static void handle_cheat_menu(void) {
  static rg_gui_option_t choices[32];
  static char choices_names[32][64];

  while (true) {
    int count = 0;
    for (int i = 0; i < 30; i++) {
      uint32 a;
      uint8 v;
      int s, t, comp;
      char *full_name = NULL;
      if (!FCEUI_GetCheat(i, &full_name, &a, &v, &comp, &s, &t))
        break;

      if (!full_name)
        continue;

      char *sep = strchr(full_name, '|');
      if (sep) {
        size_t len = sep - full_name;
        if (len > 60)
          len = 60;
        strncpy(choices_names[count], full_name, len);
        choices_names[count][len] = 0;
      } else {
        strncpy(choices_names[count], full_name, 63);
        choices_names[count][63] = 0;
      }
      char *display_name = choices_names[count];

      choices[count].flags = RG_DIALOG_FLAG_NORMAL;
      choices[count].label = display_name;
      choices[count].value = s ? "ON" : "OFF";
      choices[count].update_cb = cheat_toggle_cb;
      choices[count].arg = (intptr_t)i;
      count++;
    }

    if (count == 0) {
      rg_gui_alert("Game Genie", "No codes active. Use 'Load' or 'Add Code'.");
      break;
    }

    choices[count++] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t sel_arg = rg_gui_dialog("Game Genie", choices, last_cheat_sel);

    if (sel_arg == RG_DIALOG_CANCELLED)
      break;
  }
}

static void handle_add_cheat_menu(void) {
  char *code = rg_gui_input_str("Add Game Genie Code", "Enter Code (ABC-DEF)", "");
  if (code) {
    char *name = rg_gui_input_str("Add Game Genie Code", "Enter Description", "");
    if (name) {
      apply_cheat_code(code, name, 1);
      save_cheats();
      rg_gui_alert("Game Genie", "Code added successfully.");
      free(name);
    }
    free(code);
  }
}

static void handle_delete_cheat_menu(void) {
  static rg_gui_option_t choices[32];
  static char choices_names[32][64];

  while (true) {
    int count = 0;
    for (int i = 0; i < 30; i++) {
      uint32 a;
      uint8 v;
      int s, t, comp;
      char *full_name = NULL;
      if (!FCEUI_GetCheat(i, &full_name, &a, &v, &comp, &s, &t))
        break;

      if (!full_name)
        continue;

      char *sep = strchr(full_name, '|');
      if (sep) {
        size_t len = sep - full_name;
        if (len > 60)
          len = 60;
        strncpy(choices_names[count], full_name, len);
        choices_names[count][len] = 0;
      } else {
        strncpy(choices_names[count], full_name, 63);
        choices_names[count][63] = 0;
      }
      char *display_name = choices_names[count];

      choices[count].flags = RG_DIALOG_FLAG_NORMAL;
      choices[count].label = display_name;
      choices[count].value = NULL;
      choices[count].arg = (intptr_t)i;
      count++;
    }

    if (count == 0) {
      rg_gui_alert("Delete Code", "No codes to delete.");
      break;
    }

    choices[count++] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t sel_arg = rg_gui_dialog("Delete Code", choices, 0);

    if (sel_arg == RG_DIALOG_CANCELLED)
      break;

    if (sel_arg >= 0 && sel_arg < 30) {
      FCEUI_DelCheat((uint32)sel_arg);
      save_cheats();
      // Stay in the menu to delete more or show updated list
    }
  }
}

static rg_gui_event_t handle_load_cheats_cb(rg_gui_option_t *opt,
                                            rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    load_cheats();
    rg_gui_alert("Game Genie", "Codes loaded from SD Card.");
    return RG_DIALOG_VOID;
  }
  return RG_DIALOG_VOID;
}

static rg_gui_event_t handle_save_cheats_cb(rg_gui_option_t *opt,
                                            rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    save_cheats();
    rg_gui_alert("Game Genie", "Codes saved to SD Card.");
    return RG_DIALOG_VOID;
  }
  return RG_DIALOG_VOID;
}

static rg_gui_event_t handle_cheat_list_cb(rg_gui_option_t *opt,
                                           rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    handle_cheat_menu();
    return RG_DIALOG_VOID;
  }
  return RG_DIALOG_VOID;
}

static rg_gui_event_t handle_add_cheat_menu_cb(rg_gui_option_t *opt,
                                               rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER)
    handle_add_cheat_menu();
  return RG_DIALOG_VOID;
}

static rg_gui_event_t handle_delete_cheat_menu_cb(rg_gui_option_t *opt,
                                                  rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER)
    handle_delete_cheat_menu();
  return RG_DIALOG_VOID;
}

static rg_gui_event_t handle_cheat_menu_cb(rg_gui_option_t *opt,
                                           rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    const rg_gui_option_t choices[] = {
        {0, "Active Codes", ">", RG_DIALOG_FLAG_NORMAL, &handle_cheat_list_cb},
        {0, "Add Game Genie Code", "-", RG_DIALOG_FLAG_NORMAL,
         &handle_add_cheat_menu_cb},
        {0, "Delete Code", "-", RG_DIALOG_FLAG_NORMAL,
         &handle_delete_cheat_menu_cb},
        {0, "Load from SD", "-", RG_DIALOG_FLAG_NORMAL, &handle_load_cheats_cb},
        {0, "Save to SD", "-", RG_DIALOG_FLAG_NORMAL, &handle_save_cheats_cb},
        RG_DIALOG_END};
    rg_gui_dialog("Game Genie", choices, 0);
    save_cheats();
    return RG_DIALOG_VOID;
  }
  return RG_DIALOG_VOID;
}

static void update_palette(nespal_t type) {
  if (type < 0 || type >= NES_PALETTE_COUNT)
    type = 0;

  // FCEUMM expects 64 RGB triplets (192 bytes)
  FCEUI_SetPaletteArray((uint8_t *)nes_palettes[type]);
  palette_dirty = 3;
}

static rg_gui_event_t palette_selection_cb(rg_gui_option_t *option,
                                           rg_gui_event_t event) {
  const char *names[] = {"Nofrendo", "Composite", "Classic",
                         "NTSC",     "PVM",       "Smooth"};
  const char *config_ns = app ? app->configNs : "fceumm";
  int palette = rg_settings_get_number(config_ns, SETTING_PALETTE, 0);

  if (event == RG_DIALOG_INIT) {
    //
  } else if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT ||
             event == RG_DIALOG_ENTER || event == RG_DIALOG_SELECT) {
    if (event == RG_DIALOG_PREV)
      palette = (palette + NES_PALETTE_COUNT - 1) % NES_PALETTE_COUNT;
    else
      palette = (palette + 1) % NES_PALETTE_COUNT;

    rg_settings_set_number(config_ns, SETTING_PALETTE, palette);
    rg_settings_commit();
    update_palette((nespal_t)palette);
    save_config();
    return RG_DIALOG_REDRAW;
  }

  if (option && option->value) {
    strncpy(option->value, names[palette], 15);
    option->value[15] = 0;
  }

  return RG_DIALOG_VOID;
}

static rg_gui_event_t frameskip_cb(rg_gui_option_t *option,
                                   rg_gui_event_t event) {
  const int8_t modes[] = {0, -1, 1, 2, 3}; // Off, Auto, 1, 2, 3
  int current_idx = 0;

  for (int i = 0; i < 5; i++) {
    if (modes[i] == current_frameskip) {
      current_idx = i;
      break;
    }
  }

  if (event == RG_DIALOG_PREV)
    current_idx = (current_idx + 4) % 5;
  if (event == RG_DIALOG_NEXT)
    current_idx = (current_idx + 1) % 5;

  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    current_frameskip = modes[current_idx];
    rg_settings_set_number(NS_APP, "frameskip", current_frameskip);
    save_config();
    return RG_DIALOG_REDRAW;
  }

  if (current_frameskip == -1)
    strcpy(option->value, _("Auto"));
  else if (current_frameskip == 0)
    strcpy(option->value, _("Off"));
  else
    sprintf(option->value, "%d", current_frameskip);

  return RG_DIALOG_VOID;
}

// --- GUI CALLBACKS
static void options_handler(rg_gui_option_t *dest) {
  *dest++ = (rg_gui_option_t){.label = "Frameskip",
                              .value = "-",
                              .flags = RG_DIALOG_FLAG_NORMAL,
                              .update_cb = frameskip_cb};
  *dest++ = (rg_gui_option_t){.label = "Game Genie",
                              .flags = RG_DIALOG_FLAG_NORMAL,
                              .update_cb = handle_cheat_menu_cb};
  *dest++ = (rg_gui_option_t){.label = "Color Palette",
                              .value = "-",
                              .flags = RG_DIALOG_FLAG_NORMAL,
                              .update_cb = palette_selection_cb};
  *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

// FDS Functions from fds.h
extern bool FCEU_FDSIsDiskInserted();
extern void FCEU_FDSInsert(int oride);
extern void FCEU_FDSEject(void);
extern void FCEU_FDSSelect(void);
extern void FCEU_FDSSelect_previous(void);

// --- FCEUMM MAIN
void fceumm_main(void) {
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
  current_frameskip = -1;

  load_config();
  sync_audio_to_system(); // Initial sync for OC level

  if (!FCEUI_Initialize()) {
    RG_PANIC("FCEUI_Initialize failed");
  }

  // Load ROM into PSRAM to avoid Internal RAM pressure (especially for large
  // ROMs)
  void *rom_data = NULL;
  size_t rom_size = 0;

  // First, get the file size
  rg_stat_t st = rg_storage_stat(app->romPath);
  if (st.size > 0) {
    rom_size = st.size;
    // Load small ROMs into Internal RAM for faster access
    rom_data =
        rg_alloc(rom_size, (rom_size < 128 * 1024) ? MEM_FAST : MEM_SLOW);
    if (rom_data) {
      if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size,
                                RG_FILE_USER_BUFFER)) {
        free(rom_data);
        rom_data = NULL;
      }
    }
  }

  if (!rom_data) {
    RG_PANIC("Failed to load ROM into PSRAM");
  }

  if (!FCEUI_LoadGame(app->romPath, rom_data, rom_size, NULL)) {
    RG_PANIC("FCEUI_LoadGame failed");
  }

  load_sram();
  load_cheats();

  char *sram_path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
  if (sram_path) {
    rg_storage_mkdir(rg_dirname(sram_path));
    free(sram_path);
  }

  // Region detection
  int slstart, slend;
  int is_pal = FCEUI_GetCurrentVidSystem(&slstart, &slend);
  rg_system_set_tick_rate(is_pal ? 50 : 60);
  app->frameskip = 0;
  RG_LOGI("Detected %s region (%d lines)\n", is_pal ? "PAL" : "NTSC",
          is_pal ? 312 : 262);

  // Use normal_scanlines (updated by LoadGame) for surface height
  extern unsigned normal_scanlines;
  int surface_height = (normal_scanlines > 0 && normal_scanlines <= 312)
                           ? normal_scanlines
                           : NES_HEIGHT;

  // Use 256 lines to avoid out-of-bounds writes from FCEUMM's PPU (which can
  // write to line 240) and to accommodate PAL games safely. NES_WIDTH (256) is
  // Triple Buffering in MEM_FAST
  updates[0] = rg_surface_create(NES_WIDTH, 256, RG_PIXEL_PAL565_BE, MEM_FAST);
  updates[1] = rg_surface_create(NES_WIDTH, 256, RG_PIXEL_PAL565_BE, MEM_FAST);
  updates[2] = rg_surface_create(NES_WIDTH, 256, RG_PIXEL_PAL565_BE, MEM_FAST);
  currentBufferIndex = 0;
  currentUpdate = updates[currentBufferIndex];
  XBuf = (uint8_t *)currentUpdate->data;

  // Initialize Async Audio Pool with Queue size 1 (Strict 100% Speed Lock)
  audio_queue_full = xQueueCreate(1, sizeof(audio_msg_t *));
  audio_queue_empty = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(audio_msg_t *));

  for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
    audio_msg_t *msg = &audio_pool[i];
    xQueueSend(audio_queue_empty, &msg, 0);
  }

  rg_task_create("nes_audio", &audio_task, NULL, 3072, RG_TASK_PRIORITY_7, 0);

  // Initialize external options
  extern void FCEUI_DisableSpriteLimitation(int a);
  FCEUI_DisableSpriteLimitation(
      0); // Restore 8-sprite limit for 240MHz performance

  FSettings.soundq = 0; // Restore LQ mode for FPS stability
  FSettings.SoundVolume = 100;
  FSettings.TriangleVolume = 256;
  FSettings.SquareVolume[0] = 256;
  FSettings.SquareVolume[1] = 256;
  FSettings.NoiseVolume = 256;
  FSettings.PCMVolume = 256;
  FCEUI_Sound(app->sampleRate);
  FCEUI_SetInput(0, SI_GAMEPAD, &fceu_joystick, 0);

  update_palette(
      (nespal_t)rg_settings_get_number(app->configNs, SETTING_PALETTE, 0));

  if (app->bootFlags & RG_BOOT_RESUME) {
    rg_emu_load_state(app->saveSlot);
  }

  app->frameskip = 0;

  static uint32_t joystick_old = 0;
  static bool menu_cancelled = false;
  static bool menu_pressed = false;
  static bool turbo_a_toggled = false;
  static bool turbo_b_toggled = false;
  static int turbo_counter = 0;

  while (!rg_system_exit_called()) {

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
      // FDS: Next Side
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
      if (joystick & RG_KEY_A) {
        if (!turbo_a_toggled || (turbo_counter & 4))
          input_buf |= JOY_A;
      }
      if (joystick & RG_KEY_B) {
        if (!turbo_b_toggled || (turbo_counter & 4))
          input_buf |= JOY_B;
      }
      if (joystick & RG_KEY_START)
        input_buf |= JOY_START;
      if (joystick & RG_KEY_SELECT)
        input_buf |= JOY_SELECT;
    }

    if (joystick & RG_KEY_OPTION) {
      save_sram();
      rg_gui_options_menu();
    }

    joystick_old = joystick;
    fceu_joystick = input_buf;

    // Save config if OC changed from system menu
    static int last_oc = -1;
    if (last_oc == -1)
      last_oc = rg_system_get_overclock();
    int cur_oc = rg_system_get_overclock();
    if (cur_oc != last_oc) {
      last_oc = cur_oc;
      sync_audio_to_system(); // Immediate update for the new clock speed
      save_config();
    }

    int64_t startTime = rg_system_timer();

    static int skipFrames = 0;
    bool drawFrame = !skipFrames;

    // Switch buffers and set target for next frame
    if (drawFrame) {
      currentBufferIndex = (currentBufferIndex + 1) % 3;
      currentUpdate = updates[currentBufferIndex];
      // Copy palette only when it changes to avoid unnecessary memory traffic
      if (palette_dirty > 0) {
        memcpy(currentUpdate->palette, palette565, 512);
        palette_dirty--;
      }

      currentUpdate->width = NES_WIDTH;
      currentUpdate->height = surface_height;
      currentUpdate->offset = 0;

      XBuf = (uint8_t *)currentUpdate->data;
    }

    uint8_t *gfx = NULL;
    int32_t *sound = NULL;
    int32_t sound_samples = 0;

    // FCEUMM handles internal skipping if we pass the flag
    FCEUI_Emulate(&gfx, &sound, &sound_samples, drawFrame ? 0 : 1);

    // Measure actual emulation time (CPU work) BEFORE audio/display sync
    int64_t emulationTime = rg_system_timer() - startTime;
    rg_system_tick(emulationTime);

    // Audio submission provides the primary pacing (sync to 100% speed)
    // We submit audio BEFORE display because it's more sensitive to jitter.
    update_audio(sound, sound_samples);

    if (drawFrame && gfx) {
      rg_display_submit(currentUpdate, RG_DISPLAY_WRITE_NOSYNC);
    }

    // Reset extrascanlines for stability (Standard NES timing)
    extern unsigned extrascanlines;
    extrascanlines = 0;

    // Dynamic frame skipping logic based on actual emulation cost
    // Dynamic frame skipping logic
    if (current_frameskip == 0) {
      skipFrames = 0;
    } else if (current_frameskip > 0) {
      if (skipFrames == 0)
        skipFrames = current_frameskip;
      else
        skipFrames--;
    } else { // Auto (Prioritize 60 FPS)
      if (skipFrames == 0) {
        // Only skip if emulation is clearly falling behind ( > 105% of frame
        // time)
        if (emulationTime > (app->frameTime * 105 / 100)) {
          skipFrames = 1;
        }
      } else {
        skipFrames--;
      }
    }

    // Pacing is now strictly controlled by the 1-frame audio queue
  }

  save_sram();
  FCEUI_CloseGame();

  if (updates[0])
    rg_surface_free(updates[0]);
  if (updates[1])
    rg_surface_free(updates[1]);
  if (updates[2])
    rg_surface_free(updates[2]);
  if (rom_data)
    free(rom_data);

  updates[0] = updates[1] = updates[2] = NULL;
  rom_data = NULL;
}

extern void nofrendo_main(void);

void nes_main(void) {
  app = rg_system_get_app();

  // FDS always uses FCEUMM
  if (strcmp(app->configNs, "fds") == 0) {
    app->name = "fceumm";
    fceumm_main();
    return;
  }

  // Check boot-specific core (from launcher's choose core)
  int core = (int)rg_settings_get_number(NS_BOOT, "Core", -1);
  if (core == -1) {
    // Check preferred core for NES
    core = (int)rg_settings_get_number(NS_APP, "Core",
                                       0); // 0 = FCEUMM, 1 = Nofrendo
  }

  if (core == 1) {
    app->name = "nofrendo";
    nofrendo_main();
  } else {
    app->name = "fceumm";
    fceumm_main();
  }
}
