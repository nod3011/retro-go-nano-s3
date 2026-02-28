#include <rg_system.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern void WsInit(void);
extern void WsReset(void);
extern int WsRun(void);
extern int WsCreateFromMemory(const uint8_t *rom, size_t len);
extern int WsLoadState(const char *filename);
extern int WsSaveState(const char *filename);
extern void AllocateBuffers(void);
extern uint16_t *FrameBuffer;

// From WS.h / WSHard.h
// Buttons are active low.
// From WS.h / WSHard.h
// The oswan core expects bits to be SET when pressed (Active High internally).
// Nibble 0: Group 0x10 (Y-Pad)
// Nibble 1: Group 0x20 (X-Pad)
// Nibble 2: Group 0x40 (Buttons)
#define WS_Y1 (1 << 0)
#define WS_Y2 (1 << 1)
#define WS_Y3 (1 << 2)
#define WS_Y4 (1 << 3)
#define WS_X1 (1 << 4)
#define WS_X2 (1 << 5)
#define WS_X3 (1 << 6)
#define WS_X4 (1 << 7)
#define WS_SELECT (1 << 8)
#define WS_START (1 << 9)
#define WS_B (1 << 10)
#define WS_A (1 << 11)

#define WIDTH 224
#define HEIGHT 144

// Sound buffering
extern int16_t *sndbuffer[2];
extern int32_t rBuf;
extern int apuBufLen(void);

uint32_t lastPadState = 0;
static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *update;
static bool use_y_pad = false;

int ws_input_poll(int mode) {
  uint32_t joystick = rg_input_read_gamepad();
  uint16_t state = 0x0000; // Active High for oswan core
  static bool combo_pressed = false;

  if (joystick & RG_KEY_MENU) {
    rg_gui_game_menu();
    return lastPadState;
  }
  if (joystick & RG_KEY_OPTION) {
    rg_gui_options_menu();
    return lastPadState;
  }

  bool select_pressed = (joystick & RG_KEY_SELECT) != 0;

  // Toggle X-Pad / Y-Pad mode when Select is pressed
  if (select_pressed) {
    if (!combo_pressed) {
      use_y_pad = !use_y_pad;
      combo_pressed = true;
    }
    // Block the Select input from being sent to the emulator
    return lastPadState;
  } else {
    combo_pressed = false;
  }

  if (use_y_pad) {
    // Map D-Pad to Y-Pad
    if (joystick & RG_KEY_UP)
      state |= WS_Y1;
    if (joystick & RG_KEY_RIGHT)
      state |= WS_Y2;
    if (joystick & RG_KEY_DOWN)
      state |= WS_Y3;
    if (joystick & RG_KEY_LEFT)
      state |= WS_Y4;
  } else {
    // Map D-Pad to X-Pad
    if (joystick & RG_KEY_UP)
      state |= WS_X1;
    if (joystick & RG_KEY_RIGHT)
      state |= WS_X2;
    if (joystick & RG_KEY_DOWN)
      state |= WS_X3;
    if (joystick & RG_KEY_LEFT)
      state |= WS_X4;
  }

  // Buttons
  if (joystick & RG_KEY_A)
    state |= WS_B;
  if (joystick & RG_KEY_B)
    state |= WS_A;
  if (joystick & RG_KEY_START)
    state |= WS_START;
  if (joystick & RG_KEY_SELECT)
    state |= WS_SELECT;

  lastPadState = state;
  return state;
}

// Submit sound and video after a frame
static void IRAM_ATTR SubmitFrame(void) {
  // Swap buffers to avoid writing into the active display DMA buffer
  update = updates[update == updates[0] ? 1 : 0];

  uint8_t *dst = (uint8_t *)update->data;
  const uint16_t *src = FrameBuffer;
  for (int y = 0; y < HEIGHT; ++y) {
    memcpy(dst, src, WIDTH * sizeof(uint16_t));
    dst += update->stride;
    src += 240; // SCREEN_WIDTH from oswan is 240
  }

  rg_display_submit(update, 0);

  // Drain Audio from oswan buffer
  int have = apuBufLen();
  while (have > 0) {
    int chunk = (have > 128) ? 128 : have;
    int16_t samples[128 * 2]; // 2 channels per frame
    for (int i = 0; i < chunk; ++i) {
      int16_t L = sndbuffer[0][rBuf];
      int16_t R = sndbuffer[1][rBuf];
      rBuf = (rBuf + 1);
      if (rBuf >= 2048)
        rBuf = 0; // SND_RNGSIZE
      samples[i * 2] = L;
      samples[i * 2 + 1] = R;
    }
    rg_audio_submit((const rg_audio_frame_t *)samples, chunk);
    have -= chunk;
  }
}

// Emulated video render tick
void ws_graphics_paint(void) { SubmitFrame(); }

static bool reset_handler(bool hard) {
  WsReset();
  return true;
}

static bool screenshot_handler(const char *filename, int width, int height) {
  return rg_surface_save_image_file(update, filename, width, height);
}

static bool load_state_handler(const char *filename) {
  return WsLoadState(filename) != 0;
}

static bool save_state_handler(const char *filename) {
  return WsSaveState(filename) != 0;
}

void app_main(void) {
  const rg_handlers_t handlers = {
      .reset = &reset_handler,
      .screenshot = &screenshot_handler,
      .loadState = &load_state_handler,
      .saveState = &save_state_handler,
  };

  app = rg_system_init(48000, &handlers, NULL);
  rg_system_set_tick_rate(75);

  updates[0] = rg_surface_create(WIDTH, HEIGHT, RG_PIXEL_565_LE, MEM_FAST);
  updates[1] = rg_surface_create(WIDTH, HEIGHT, RG_PIXEL_565_LE, MEM_FAST);
  update = updates[0];

  // Map internal oswan buffer directly? Emulators buffer might be incompatible.
  // Instead we will copy during SubmitFrame or map directly.
  AllocateBuffers();
  WsInit();

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

  if (WsCreateFromMemory(rom_ptr, rom_size) != 0) {
    rg_gui_alert("Error", "Failed to load ROM.");
    rg_system_exit();
  }

  if (app->bootFlags & RG_BOOT_RESUME) {
    rg_emu_load_state(app->saveSlot);
  }

  for (;;) {
    WsRun();

    // Input is polled synchronously by WsRun, we just need to pass the frame
  }

  rg_system_exit();
}
