#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rg_system.h"

#define CZ80 1
#define RETRO_COMPAT_IMPLEMENTATION
#include "graphics.h"
#include "input.h"
#include "race-memory.h"
#include "retro_compat.h"
#include "tlcs900h.h"
#include "types.h"

int is_mono_game = 0;

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

static void set_defaults_after_boot(void) {
  // Color/mono based on ROM
  uint8_t console_type = tlcsMemReadB(0x00200023);
  tlcsMemWriteB(0x00006F91,
                console_type); // Set BIOS mode (0x11=Color, 0x10=Mono)

  if (tipo_consola == 1)
    tlcsMemWriteB(0x00006F91, 0x00); // force mono override manually if set

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

static void poll_input(void) {
  uint32_t state = rg_input_read_gamepad();
  int joy = 0;

  if (state & RG_KEY_UP)
    joy |= (1 << 0);
  if (state & RG_KEY_DOWN)
    joy |= (1 << 1);
  if (state & RG_KEY_LEFT)
    joy |= (1 << 2);
  if (state & RG_KEY_RIGHT)
    joy |= (1 << 3);
  if (state & RG_KEY_A)
    joy |= (1 << 4);
  if (state & RG_KEY_B)
    joy |= (1 << 5);
  if (state & (RG_KEY_START | RG_KEY_SELECT | RG_KEY_OPTION))
    joy |= (1 << 6);
  if (state & RG_KEY_MENU)
    joy |= (1 << 6);

  ngpInputState = (unsigned char)joy;
}

static bool reset_handler(bool hard) {
  if (hard)
    return false;
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

static void event_handler(int event, void *arg) {
  if (event == RG_EVENT_SHUTDOWN || event == RG_EVENT_SLEEP) {
    extern unsigned char needToWriteFile;
    extern void writeSaveGameFile(void);
    if (needToWriteFile)
      writeSaveGameFile();
  }
}

static bool load_state_handler(const char *filename) {
  size_t size = 0;
  void *data = NULL;

  if (!rg_storage_read_file(filename, &data, &size, 0))
    return false;

  if (size == state_get_size()) {
    state_restore_mem(data);
  }
  free(data);
  return true;
}

static bool screenshot_handler(const char *filename, int width, int height) {
  return rg_surface_save_image_file(update, filename, width, height);
}

// Audio Buffers
#define kSampleRate 22050
#define kFps 60
#define kChunk ((kSampleRate + kFps / 2) / kFps) // 368
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

  int16_t samples[368 * 2];
  for (int i = 0; i < kChunk; ++i) {
    int32_t a = (int16_t)s_psg[i];
    int32_t b = (int16_t)s_dac[i];
    int32_t mixed = a + b;
    if (mixed > 32767)
      mixed = 32767;
    if (mixed < -32768)
      mixed = -32768;
    samples[i * 2] = (int16_t)mixed;
    samples[i * 2 + 1] = (int16_t)mixed;
  }
  rg_audio_submit((const rg_audio_frame_t *)samples, kChunk);
}

void app_main() {
  rg_handlers_t handlers = {
      .reset = reset_handler,
      .saveState = save_state_handler,
      .loadState = load_state_handler,
      .screenshot = screenshot_handler,
      .event = event_handler,
  };
  rg_system_init(22050, &handlers, NULL);
  app = rg_system_get_app();

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

  // Force Color BIOS mode via RAM register 0x6F91
  tlcsMemWriteB(0x00006F91, 0x10);

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

  // Display
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

  rg_system_set_tick_rate(1);

  if (app->bootFlags & RG_BOOT_RESUME) {
    rg_emu_load_state(app->saveSlot);
  }

  bool menuCancelled = false;
  bool menuPressed = false;
  int64_t sram_save_timer = 0;

  while (m_bIsActive) {
    uint32_t joystick = rg_input_read_gamepad();

    if (menuPressed && !(joystick & RG_KEY_MENU)) {
      if (!menuCancelled) {
        rg_task_delay(50);
        rg_gui_game_menu();
      }
      menuCancelled = false;
    } else if (joystick & RG_KEY_OPTION) {
      rg_gui_options_menu();
    }

    menuPressed = joystick & RG_KEY_MENU;

    if (menuPressed && joystick & ~RG_KEY_MENU) {
      menuCancelled = true;
    }

    poll_input();

    tlcs_execute(5700000 / 60);

    if (g_frame_ready) {
      g_frame_ready = 0;
      submit_frame();
    }

    extern unsigned char needToWriteFile;
    if (needToWriteFile && rg_system_timer() > sram_save_timer) {
      extern void writeSaveGameFile(void);
      writeSaveGameFile();
      sram_save_timer = rg_system_timer() + 2 * 1000 * 1000;
    }
  }

  rg_system_exit();
}
