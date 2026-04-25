#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "rg_system.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define CZ80 1
#define RETRO_COMPAT_IMPLEMENTATION
#include "graphics.h"
#include "input.h"
#include "race-memory.h"
#include "retro_compat.h"
#include "tlcs900h.h"
#include "types.h"

#define kSampleRate 22050
#define kFps 60
#define kChunk ((kSampleRate + kFps / 2) / kFps) // 368
#define AUDIO_BUFFER_COUNT 2

typedef struct {
    rg_audio_frame_t frames[kChunk];
    size_t count;
} audio_msg_t;

static audio_msg_t audio_pool[AUDIO_BUFFER_COUNT];
static QueueHandle_t audio_queue_full;
static QueueHandle_t audio_queue_empty;

static void audio_task(void *arg) {
    audio_msg_t *msg;
    while (xQueueReceive(audio_queue_full, &msg, portMAX_DELAY)) {
        if (msg == (audio_msg_t *)-1) break; // Shutdown signal
        rg_audio_submit(msg->frames, msg->count);
        xQueueSend(audio_queue_empty, &msg, portMAX_DELAY);
    }
    vTaskDelete(NULL);
}

int is_mono_game = 0;
#define NGP_CPU_CLOCK 6144000

// Defined in retro_compat.h (inside RETRO_COMPAT_IMPLEMENTATION)
// EMUINFO m_emuInfo;
// struct ngp_screen *screen;

int tipo_consola = 0;
char retro_save_directory[3] = {0};
int gfx_hacks = 0;

extern void Z80_Reset(void);
extern void tlcs_reinit(void);
extern void Z80_Init(void);

// Audio
extern void sound_init(int SampleRate);
extern void sound_update(uint16_t *chip_buffer, int length_bytes);
extern void dac_update(uint16_t *dac_buffer, int length_bytes);
extern int Cz80_allocate_flag_tables(void);
extern void Cz80_free_flag_tables(void);
extern void graphics_free(void);
extern void audio_dac_free(void);

// Config
typedef struct {
    int overclock;
    int frameskip;
    int btn_a_map;   // Physical key for NGP A
    int btn_b_map;   // Physical key for NGP B
} ngp_config_t;

static ngp_config_t config;

static const char *btn_names[] = {"A", "B"};
static const uint32_t btn_keys[] = {RG_KEY_A, RG_KEY_B};

static void load_config() {
    config.overclock = (int)rg_settings_get_number(NS_FILE, "overclock", 2);
    config.frameskip = (int)rg_settings_get_number(NS_FILE, "frameskip", 0);
    config.btn_a_map = (int)rg_settings_get_number(NS_FILE, "btn_a_map", 0); // Default A
    config.btn_b_map = (int)rg_settings_get_number(NS_FILE, "btn_b_map", 1); // Default B
}

static void save_config() {
    rg_settings_set_number(NS_FILE, "overclock", config.overclock);
    rg_settings_set_number(NS_FILE, "frameskip", config.frameskip);
    rg_settings_set_number(NS_FILE, "btn_a_map", config.btn_a_map);
    rg_settings_set_number(NS_FILE, "btn_b_map", config.btn_b_map);
    rg_settings_commit();
}

// Graphics / VDP pointers
unsigned short *drawBuffer = NULL;
volatile unsigned g_frame_ready = 0;
extern int finscan;

// Race State
extern int state_get_size(void);
extern int state_store_mem(void *state);
extern int state_restore_mem(void *state);

unsigned char *rasterY = 0;
unsigned char *frame0Pri = 0;
unsigned char *frame1Pri = 0;
unsigned char *color_switch = 0;
unsigned char *scanlineY = 0;
unsigned char *scrollSpriteX = 0;
unsigned char *scrollSpriteY = 0;
unsigned char *sprite_palette_numbers = 0;
unsigned char *sprite_table = 0;
unsigned short *patterns = 0;
unsigned char *oowSelect = 0;
unsigned short *oowTable = 0;
unsigned char *wndTopLeftY = 0;
unsigned char *wndSizeY = 0;
unsigned char *wndSizeX = 0;
unsigned char *bgSelect = 0;
unsigned char *bw_palette_table = 0;
unsigned short *palette_table = 0;
unsigned short *bgTable = 0;
unsigned char *wndTopLeftX = 0;
unsigned char *scrollFrontY = 0;
unsigned char *scrollFrontX = 0;
unsigned short *tile_table_front = 0;
unsigned char *scrollBackY = 0;
unsigned short *tile_table_back = 0;
unsigned char *scrollBackX = 0;
unsigned char *pattern_table = NULL;

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *update;
int m_bIsActive = 1;

// Turbo logic state
static bool turbo_btn_a_enabled = false;
static bool turbo_btn_b_enabled = false;
static bool menu_cancelled = false;
static bool menu_combo_a = false; // Track if A was pressed during Menu hold
static bool menu_combo_b = false; // Track if B was pressed during Menu hold
static int turbo_counter = 0;

static void set_defaults_after_boot(void) {
  // Color/mono based on ROM (Header 0x23)
  uint8_t console_type = tlcsMemReadB(0x00200023);
  tlcsMemWriteB(0x00006F91, (console_type == 0x00) ? 0x10 : console_type); 

  // ROM Language
  tlcsMemWriteB(0x00006F87, 0x01); // English

  // Video IRQ
  tlcsMemWriteB(0x00004000, tlcsMemReadB(0x00004000) | 0xC0);

  // Power Bits
  tlcsMemWriteB(0x00006F84, 0x40);
  tlcsMemWriteB(0x00006F85, 0x00);
  tlcsMemWriteB(0x00006F86, 0x00);
}

static void map_vdp_tables_full() {
  sprite_table = (unsigned char *)get_address(0x00008800);
  pattern_table = (unsigned char *)get_address(0x0000A000);
  patterns = (unsigned short *)pattern_table;
  tile_table_front = (unsigned short *)get_address(0x00009000);
  tile_table_back = (unsigned short *)get_address(0x00009800);
  palette_table = (unsigned short *)get_address(0x00008200);
  bw_palette_table = (unsigned char *)get_address(0x00008100);
  sprite_palette_numbers = (unsigned char *)get_address(0x00008C00);

  scanlineY = (unsigned char *)get_address(0x00008009);
  frame0Pri = (unsigned char *)get_address(0x00008000);
  frame1Pri = (unsigned char *)get_address(0x00008030);

  wndTopLeftX = (unsigned char *)get_address(0x00008002);
  wndTopLeftY = (unsigned char *)get_address(0x00008003);
  wndSizeX = (unsigned char *)get_address(0x00008004);
  wndSizeY = (unsigned char *)get_address(0x00008005);

  scrollSpriteX = (unsigned char *)get_address(0x00008020);
  scrollSpriteY = (unsigned char *)get_address(0x00008021);
  scrollFrontX = (unsigned char *)get_address(0x00008032);
  scrollFrontY = (unsigned char *)get_address(0x00008033);
  scrollBackX = (unsigned char *)get_address(0x00008034);
  scrollBackY = (unsigned char *)get_address(0x00008035);

  bgSelect = (unsigned char *)get_address(0x00008118);
  bgTable = (unsigned short *)get_address(0x000083E0);
  oowSelect = (unsigned char *)get_address(0x00008012);
  oowTable = (unsigned short *)get_address(0x000083F0);

  color_switch = (unsigned char *)get_address(0x00006F91);

  static unsigned char s_dummy_scan = 0;
  if (!scanlineY)
    scanlineY = &s_dummy_scan;
  rasterY = scanlineY;
}

// Input states
// We need to map inputs somewhere but let's implement the loop first

static void poll_input(uint32_t state) {
  int joy = 0;

  if (state & RG_KEY_UP) joy |= (1 << 0);
  if (state & RG_KEY_DOWN) joy |= (1 << 1);
  if (state & RG_KEY_LEFT) joy |= (1 << 2);
  if (state & RG_KEY_RIGHT) joy |= (1 << 3);

  uint32_t key_a = btn_keys[config.btn_a_map];
  uint32_t key_b = btn_keys[config.btn_b_map];

  if (state & key_a) {
    bool turbo = (config.btn_a_map == 0) ? turbo_btn_a_enabled : turbo_btn_b_enabled;
    if (!turbo || (turbo_counter & 4)) joy |= (1 << 4);
  }
  if (state & key_b) {
    bool turbo = (config.btn_b_map == 0) ? turbo_btn_a_enabled : turbo_btn_b_enabled;
    if (!turbo || (turbo_counter & 4)) joy |= (1 << 5);
  }

  if (state & (RG_KEY_START | RG_KEY_SELECT))
    joy |= (1 << 6);

  ngpInputState = (unsigned char)joy;
}

static bool reset_handler(bool hard) {
  if (hard) {
    rg_system_restart();
  }
  tlcs_reinit();
  Z80_Reset();
  return true;
}

static bool save_state_handler(const char *filename) {
  extern unsigned char needToWriteFile;
  extern void writeSaveGameFile(void);
  if (needToWriteFile)
    writeSaveGameFile();

  int size = state_get_size();
  void *data = malloc(size);
  if (!data)
    return false;

  state_store_mem(data);
  bool success = rg_storage_write_file(filename, data, size, 0);
  free(data);
  return success;
}

static rg_gui_event_t overclock_cb(rg_gui_option_t *option, rg_gui_event_t event) {
    const char *names[] = {"Disabled", "1.1x", "1.25x", "1.5x"};
    if (event == RG_DIALOG_PREV) config.overclock = (config.overclock + 3) % 4;
    if (event == RG_DIALOG_NEXT) config.overclock = (config.overclock + 1) % 4;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        rg_system_set_overclock(config.overclock);
        save_config();
    }
    strcpy(option->value, names[config.overclock]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t frameskip_cb(rg_gui_option_t *option, rg_gui_event_t event) {
    const char *names[] = {"Auto", "None", "1", "2", "3", "4"};
    if (event == RG_DIALOG_PREV) config.frameskip = (config.frameskip + 5) % 6;
    if (event == RG_DIALOG_NEXT) config.frameskip = (config.frameskip + 1) % 6;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        app->frameskip = (config.frameskip == 0) ? -1 : (config.frameskip - 1);
        save_config();
    }
    strcpy(option->value, names[config.frameskip]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t sub_btn_mapping_cb(rg_gui_option_t *option, rg_gui_event_t event) {
    int *val = (int *)option->arg;
    if (event == RG_DIALOG_PREV) *val = (*val + 1) % 2;
    if (event == RG_DIALOG_NEXT) *val = (*val + 1) % 2;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        save_config();
    }
    strcpy(option->value, btn_names[*val]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t btn_mapping_cb(rg_gui_option_t *option, rg_gui_event_t event) {
    if (event == RG_DIALOG_ENTER) {
        rg_gui_option_t options[3];
        options[0] = (rg_gui_option_t){(intptr_t)&config.btn_a_map, "NGP Button A", "-", RG_DIALOG_FLAG_NORMAL, &sub_btn_mapping_cb};
        options[1] = (rg_gui_option_t){(intptr_t)&config.btn_b_map, "NGP Button B", "-", RG_DIALOG_FLAG_NORMAL, &sub_btn_mapping_cb};
        options[2] = (rg_gui_option_t)RG_DIALOG_END;
        rg_gui_dialog(option->label, options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest) {
    *dest++ = (rg_gui_option_t){0, "Overclock", "-", RG_DIALOG_FLAG_NORMAL, &overclock_cb};
    *dest++ = (rg_gui_option_t){0, "Frameskip", "-", RG_DIALOG_FLAG_NORMAL, &frameskip_cb};
    *dest++ = (rg_gui_option_t){0, "Map Buttons", "...", RG_DIALOG_FLAG_NORMAL, &btn_mapping_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

static void event_handler(int event, void *arg) {
    if (event == RG_EVENT_SHUTDOWN || event == RG_EVENT_SLEEP) {
        extern void flashShutdown(void);
        flashShutdown();
    }
}

static bool load_state_handler(const char *filename) {
  size_t size = 0;
  void *data = NULL;

  if (!rg_storage_read_file(filename, &data, &size, 0))
    return false;

  if (size == state_get_size()) {
    RG_LOGI("Loading state: size matched %d\n", (int)size);
    rg_audio_set_mute(true);
    vTaskDelay(pdMS_TO_TICKS(50)); // Allow sound thread to settle longer
    state_restore_mem(data);
    rg_audio_set_mute(false);
    RG_LOGI("State restore complete.\n");
  } else {
    RG_LOGE("State size mismatch: expected %d, got %d\n", (int)state_get_size(),
            (int)size);
    free(data);
    return false;
  }
  free(data);
  return true;
}

static bool screenshot_handler(const char *filename, int width, int height) {
  return rg_surface_save_image_file(update, filename, width, height);
}

// Audio Buffers
static uint16_t s_psg[368];
static uint16_t s_dac[368];

static void submit_frame() {
  rg_display_submit(update, 0);

  // Swap buffers to avoid writing into the active display DMA buffer
  update = updates[update == updates[0] ? 1 : 0];
  drawBuffer = (uint16_t *)update->data;

  int lenB = kChunk * sizeof(uint16_t);
  sound_update(s_psg, lenB);
  dac_update(s_dac, lenB);

  audio_msg_t *msg;
  if (xQueueReceive(audio_queue_empty, &msg, 0) == pdTRUE) {
    for (int i = 0; i < kChunk; ++i) {
      int32_t a = (int16_t)s_psg[i];
      int32_t b = (int16_t)s_dac[i];
      int32_t mixed = a + b;
      if (mixed > 32767) mixed = 32767;
      if (mixed < -32768) mixed = -32768;
      msg->frames[i].left = (int16_t)mixed;
      msg->frames[i].right = (int16_t)mixed;
    }
    msg->count = kChunk;
    xQueueSend(audio_queue_full, &msg, portMAX_DELAY);
  }
}

void app_main() {
  rg_handlers_t handlers = {
      .reset = reset_handler,
      .saveState = save_state_handler,
      .loadState = load_state_handler,
      .screenshot = screenshot_handler,
      .event = event_handler,
      .options = options_handler,
  };
  rg_system_init(22050, &handlers, event_handler);
  
  load_config();
  rg_system_set_tick_rate(60);
  rg_system_set_overclock(config.overclock);

  // Initialize Async Audio Pool on Core 0
  audio_queue_full = xQueueCreate(1, sizeof(audio_msg_t *));
  audio_queue_empty = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(audio_msg_t *));
  for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
    audio_msg_t *msg = &audio_pool[i];
    xQueueSend(audio_queue_empty, &msg, 0);
  }
  xTaskCreatePinnedToCore(&audio_task, "audio_task", 2048, NULL, 5, NULL, 0);

  app = rg_system_get_app();
  app->screenSync = 1;
  app->frameskip = (config.frameskip == 0) ? -1 : (config.frameskip - 1);
  g_overclock = 0;

  // Load ROM
  size_t rom_size = 0;
  void *rom_ptr = NULL;

  if (rg_extension_match(app->romPath, "zip")) {
    if (!rg_storage_unzip_file(app->romPath, NULL, &rom_ptr, &rom_size,
                               RG_FILE_ALIGN_64KB))
      RG_PANIC("ROM file unzipping failed!");
  } else if (!rg_storage_read_file(app->romPath, &rom_ptr, &rom_size,
                                   RG_FILE_ALIGN_64KB)) {
    RG_PANIC("ROM load failed!");
  }

  // Init Race core
  ngp_mem_set_rom(rom_ptr, rom_size);
  m_emuInfo.romSize = (int)rom_size;

  // Simulation Mode: Always use NGPC (1) engine
  m_emuInfo.machine = 1;

  // Detect Monochrome games (Header 0x00 at 0x23). 0x10 is Color.
  uint8_t console_type = ((uint8_t *)rom_ptr)[0x23];
  is_mono_game = (console_type == 0x00);

  tipo_consola = 0;

  Cz80_allocate_flag_tables();
  ngp_mem_init();
  map_vdp_tables_full();

  extern void setFlashSize(unsigned int);
  setFlashSize((unsigned int)rom_size);

  if (bgTable)
    bgTable[0] = 0xFFFF;
  if (bgSelect)
    *bgSelect |= 0x80;

  tlcs_init();
  tlcs_reinit();
  Z80_Init();
  Z80_Reset();
  extern void audio_dac_init(void);
  audio_dac_init();
  sound_init(kSampleRate);

  set_defaults_after_boot();

  // Display - use default LE format; driver will swap bytes for BGR LCD.
  updates[0] = rg_surface_create(160, 152, RG_PIXEL_565_LE, MEM_FAST);
  updates[1] = rg_surface_create(160, 152, RG_PIXEL_565_LE, MEM_FAST);
  update = updates[0];
  drawBuffer = (uint16_t *)update->data;
  graphics_init();

  if (wndTopLeftX && wndTopLeftY && wndSizeX && wndSizeY) {
    if (*wndSizeX == 0 || *wndSizeY == 0) {
      *wndTopLeftX = 0;
      *wndTopLeftY = 0;
      *wndSizeX = 160;
      *wndSizeY = 152;
    }
  }
  if (bgSelect)
    *bgSelect |= 0x80;
  if (frame0Pri)
    *frame0Pri |= 0x80 | 0x40;

  tlcsMemWriteB(0x00004000, tlcsMemReadB(0x00004000) | 0xC0);

  finscan = 198;
  if (mainrom && (mainrom[0x000020] == 0x65 || mainrom[0x000020] == 0x93))
    finscan = 199;

  tlcsMemWriteB(0x00004000, 0xC0);

  // Kludges
  switch (tlcsMemReadW(0x00200020)) {
  case 0x0059:
  case 0x0061:
    tlcsMemWriteB(0x0020001F, 0xFF);
    break;
  }

  if (app->bootFlags & RG_BOOT_RESUME) {
    rg_emu_load_state(app->saveSlot);
  }

  static uint32_t joystick_old = 0;
  static bool menu_pressed = false;
  int64_t sram_save_timer = 0;
  int64_t frame_start = rg_system_timer();

  while (!rg_system_exit_called()) {
    uint32_t joystick = rg_input_read_gamepad();
    uint32_t joystick_down = joystick & ~joystick_old;

    if (joystick & RG_KEY_MENU) {
      if (joystick & RG_KEY_A) {
        menu_combo_a = true;
        menu_cancelled = true;
      }
      if (joystick & RG_KEY_B) {
        menu_combo_b = true;
        menu_cancelled = true;
      }
      if (joystick & ~RG_KEY_MENU) {
        menu_cancelled = true;
      }
      menu_pressed = true;
    } else {
      if (joystick_old & RG_KEY_MENU) {
        if (menu_combo_a) {
          turbo_btn_a_enabled = !turbo_btn_a_enabled;
          RG_LOGI("Physical A Turbo: %s\n", turbo_btn_a_enabled ? "ON" : "OFF");
        }
        if (menu_combo_b) {
          turbo_btn_b_enabled = !turbo_btn_b_enabled;
          RG_LOGI("Physical B Turbo: %s\n", turbo_btn_b_enabled ? "ON" : "OFF");
        }
        
        if (!menu_cancelled) {
          rg_task_delay(50);
          rg_gui_game_menu();
        }
        menu_combo_a = false;
        menu_combo_b = false;
        menu_cancelled = false;
      }
      menu_pressed = false;
    }

    if (joystick_down & RG_KEY_OPTION) {
      rg_gui_options_menu();
    }

    poll_input(menu_pressed ? 0 : joystick);

    tlcs_execute(NGP_CPU_CLOCK / 60);

    if (g_frame_ready) {
      g_frame_ready = 0;
      turbo_counter++; // Increment turbo once per frame
      submit_frame();
      int64_t now = rg_system_timer();
      rg_system_tick(now - frame_start);
      frame_start = now;
    }

    extern unsigned char needToWriteFile;
    if (needToWriteFile && rg_system_timer() > sram_save_timer) {
      extern void writeSaveGameFile(void);
      writeSaveGameFile();
      sram_save_timer = rg_system_timer() + 2 * 1000 * 1000;
    }
    joystick_old = joystick;
  }

  // Signal audio task to shutdown
  audio_msg_t *shutdown_msg = (audio_msg_t *)-1;
  xQueueSend(audio_queue_full, &shutdown_msg, 0);

  if (rom_ptr)
    free(rom_ptr);

  ngp_mem_free();
  graphics_free();
  audio_dac_free();
  Cz80_free_flag_tables();

  rg_surface_free(updates[0]);
  rg_surface_free(updates[1]);

  if (audio_queue_full) vQueueDelete(audio_queue_full);
  if (audio_queue_empty) vQueueDelete(audio_queue_empty);

  rg_system_exit();
}
