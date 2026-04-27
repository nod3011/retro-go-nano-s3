#include <gnuboy.h>
#include <cheat.h>
#include <rg_system.h>
#include <sys/time.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define AUDIO_SAMPLE_RATE (44100)
#define AUDIO_BUFFER_COUNT 2
#define AUDIO_BUFFER_SIZE 1024

typedef struct {
    rg_audio_frame_t frames[AUDIO_BUFFER_SIZE]; // Stereo S16
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

static int skipFrames = 0;
static bool slowFrame = false;
static int video_time;
static int audio_time;

static const char *sramFile;
static int autoSaveSRAM = 0;
static int autoSaveSRAM_Timer = 0;
static bool useSystemTime = true;
static bool loadBIOSFile = false;

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;

static const char *SETTING_SAVESRAM = "SaveSRAM";
static const char *SETTING_PALETTE = "Palette";
static const char *SETTING_SYSTIME = "SysTime";
static const char *SETTING_LOADBIOS = "LoadBIOS";

// --- MAIN

#ifdef RG_ENABLE_NETPLAY
static const bool gbLinkDebug = false;

typedef struct {
  uint8_t tx;
  uint8_t _pad[3];
} gb_link_packet_t;

static byte gb_link_serial_exchange(byte outgoing) {
  if (rg_netplay_status() != NETPLAY_STATUS_CONNECTED)
    return 0xFF;

  gb_link_packet_t local = {.tx = outgoing};
  gb_link_packet_t remote = {.tx = 0xFF};
  int64_t sync_start = rg_system_timer();

  // Full synchronous exchange. This blocks until the network operation is
  // complete. For Pokemon, this is exactly what we need to ensure bit-perfect
  // sync.
  rg_netplay_sync_ex(&local, &remote, sizeof(local), 0);

  int64_t sync_duration = rg_system_timer() - sync_start;

  if (gbLinkDebug || (sync_duration > 50000)) {
    RG_LOGI("gb-link: tx=%02X rx=%02X dt=%dus\n", outgoing, remote.tx,
            (int)sync_duration);
  }

  return remote.tx;
}

static bool gb_link_serial_poll(byte tx, byte *rx) {
  if (rg_netplay_status() != NETPLAY_STATUS_CONNECTED)
    return false;

  gb_link_packet_t local = {.tx = tx};
  gb_link_packet_t remote = {.tx = 0xFF};
  if (rg_netplay_poll_sync(&local, &remote, sizeof(local))) {
    *rx = remote.tx;
    return true;
  }
  return false;
}

#endif

static void update_rtc_time(void) {
  if (!useSystemTime)
    return;
  time_t timer = time(NULL);
  struct tm *info = localtime(&timer);
  gnuboy_set_time(info->tm_yday, info->tm_hour, info->tm_min, info->tm_sec);
}

static void event_handler(int event, void *arg) {
  if (event == RG_EVENT_REDRAW) {
    rg_display_submit(currentUpdate, 0);
  }
}

static bool screenshot_handler(const char *filename, int width, int height) {
  return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename) {
  return gnuboy_save_state(filename) == 0;
}

static bool load_state_handler(const char *filename) {
  if (gnuboy_load_state(filename) != 0) {
    gnuboy_reset(true);
    gnuboy_load_sram(sramFile);
    update_rtc_time();
    return false;
  }

  update_rtc_time();
  skipFrames = 0;
  autoSaveSRAM_Timer = 0;
  return true;
}

static bool reset_handler(bool hard) {
  gnuboy_reset(hard);
  update_rtc_time();
  skipFrames = 0;
  autoSaveSRAM_Timer = 0;
  return true;
}

static rg_gui_event_t palette_update_cb(rg_gui_option_t *option,
                                        rg_gui_event_t event) {
  if (gnuboy_get_hwtype() == GB_HW_CGB) {
    strcpy(option->value, "GBC");
    return RG_DIALOG_VOID;
  }

  int pal = gnuboy_get_palette();
  int max = GB_PALETTE_COUNT - 1;

  if (event == RG_DIALOG_PREV)
    pal = pal > 0 ? pal - 1 : max;

  if (event == RG_DIALOG_NEXT)
    pal = pal < max ? pal + 1 : 0;

  if (pal != gnuboy_get_palette()) {
    rg_settings_set_number(NS_APP, SETTING_PALETTE, pal);
    gnuboy_set_palette(pal);
    gnuboy_run(true);
    return RG_DIALOG_REDRAW;
  }

  if (pal == GB_PALETTE_DMG)
    strcpy(option->value, "DMG   ");
  else if (pal == GB_PALETTE_MGB0)
    strcpy(option->value, "Pocket");
  else if (pal == GB_PALETTE_MGB1)
    strcpy(option->value, "Light ");
  else if (pal == GB_PALETTE_CGB)
    strcpy(option->value, "GBC   ");
  else if (pal == GB_PALETTE_SGB)
    strcpy(option->value, "SGB   ");
  else
    sprintf(option->value, "%d/%d   ", pal + 1, max - 1);

  return RG_DIALOG_VOID;
}

static rg_gui_event_t sram_autosave_cb(rg_gui_option_t *option,
                                       rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV)
    autoSaveSRAM--;
  if (event == RG_DIALOG_NEXT)
    autoSaveSRAM++;

  autoSaveSRAM = RG_MIN(RG_MAX(0, autoSaveSRAM), 999);

  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    rg_settings_set_number(NS_APP, SETTING_SAVESRAM, autoSaveSRAM);
  }

  if (autoSaveSRAM == 0)
    strcpy(option->value, _("Off"));
  else
    sprintf(option->value, "%3ds", autoSaveSRAM);

  return RG_DIALOG_VOID;
}

static rg_gui_event_t enable_bios_cb(rg_gui_option_t *option,
                                     rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    loadBIOSFile = !loadBIOSFile;
    rg_settings_set_number(NS_APP, SETTING_LOADBIOS, loadBIOSFile);
  }
  strcpy(option->value, loadBIOSFile ? _("Yes") : _("No"));
  return RG_DIALOG_VOID;
}

#ifdef RG_ENABLE_NETPLAY
static rg_gui_event_t netplay_start_cb(rg_gui_option_t *option,
                                       rg_gui_event_t event) {
  netplay_status_t status = rg_netplay_status();

  if (event == RG_DIALOG_ENTER) {
    if (status == NETPLAY_STATUS_CONNECTED) {
      return RG_DIALOG_VOID;
    }
    if (rg_netplay_quick_start()) {
      return RG_DIALOG_SELECT;
    }
  }

  if (status == NETPLAY_STATUS_CONNECTED) {
    strcpy(option->value, _("Connected"));
  } else if (status >= NETPLAY_STATUS_CONNECTING) {
    strcpy(option->value, _("Connecting..."));
  } else {
    strcpy(option->value, _("Start"));
  }

  return RG_DIALOG_VOID;
}
#endif

static rg_gui_event_t rtc_t_update_cb(rg_gui_option_t *option,
                                      rg_gui_event_t event) {
  int d, h, m, s;
  gnuboy_get_time(&d, &h, &m, &s);

  if (option->arg == 'd') {
    if (event == RG_DIALOG_PREV && --d < 0) d = 364;
    if (event == RG_DIALOG_NEXT && ++d > 364) d = 0;
    sprintf(option->value, "%02d", d);
  }
  if (option->arg == 'h') {
    if (event == RG_DIALOG_PREV && --h < 0) h = 23;
    if (event == RG_DIALOG_NEXT && ++h > 23) h = 0;
    sprintf(option->value, "%02d", h);
  }
  if (option->arg == 'm') {
    if (event == RG_DIALOG_PREV && --m < 0) m = 59;
    if (event == RG_DIALOG_NEXT && ++m > 59) m = 0;
    sprintf(option->value, "%02d", m);
  }
  if (option->arg == 's') {
    if (event == RG_DIALOG_PREV && --s < 0) s = 59;
    if (event == RG_DIALOG_NEXT && ++s > 59) s = 0;
    sprintf(option->value, "%02d", s);
  }
  if (option->arg == 'x') {
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
      useSystemTime = !useSystemTime;
      rg_settings_set_number(NS_APP, SETTING_SYSTIME, useSystemTime);
    }
    strcpy(option->value, useSystemTime ? _("Yes") : _("No"));
  }

  gnuboy_set_time(d, h, m, s);
  return RG_DIALOG_VOID;
}

static rg_gui_event_t rtc_update_cb(rg_gui_option_t *option,
                                    rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    const rg_gui_option_t choices[] = {
        {'d', _("Day"), "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
        {'h', _("Hour"), "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
        {'m', _("Min"), "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
        {'s', _("Sec"), "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
        {'x', _("Sync"), "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
        RG_DIALOG_END};
    rg_gui_dialog(option->label, choices, 0);
  }
  int h, m;
  gnuboy_get_time(NULL, &h, &m, NULL);
  sprintf(option->value, "%02d:%02d", h, m);
  return RG_DIALOG_VOID;
}

static void video_callback(void *buffer) {
  int64_t startTime = rg_system_timer();
  slowFrame = !rg_display_sync(false);
  rg_display_submit(currentUpdate, 0);
  video_time += rg_system_timer() - startTime;
}

// --- CHEATS

static void apply_cheat_code(const char *code, const char *name, bool status) {
  uint16_t addr;
  uint8_t val;

  if (!gb_cheat_decode_gs(code, &addr, &val)) {
    RG_LOGE("Invalid GameShark code: %s\n", code);
    return;
  }

  // Use description format: "NAME|CODE"
  char full_desc[128];
  snprintf(full_desc, sizeof(full_desc), "%s|%s", name ? name : "Cheat", code);
  gb_cheat_add(full_desc, addr, val, status);
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

  gb_cheat_reset();

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
    uint16_t a;
    uint8_t v;
    bool s;
    char *full_name = NULL;
    if (!gb_cheat_get(i, &full_name, &a, &v, &s))
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
  uint16_t a;
  uint8_t v;
  bool s;
  char *name = NULL;

  if (event == RG_DIALOG_INIT || event == RG_DIALOG_UPDATE) {
    if (opt->value && gb_cheat_get(index, &name, &a, &v, &s)) {
      strcpy(opt->value, s ? _("On") : _("Off"));
    }
    return RG_DIALOG_VOID;
  }

  if (event != RG_DIALOG_ENTER && event != RG_DIALOG_SELECT)
    return RG_DIALOG_VOID;

  if (gb_cheat_get(index, &name, &a, &v, &s)) {
    gb_cheat_set(index, !s);
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
      uint16_t a;
      uint8_t v;
      bool s;
      char *full_name = NULL;
      if (!gb_cheat_get(i, &full_name, &a, &v, &s))
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
      choices[count].value = (char *)(s ? _("On") : _("Off"));
      choices[count].update_cb = cheat_toggle_cb;
      choices[count].arg = (intptr_t)i;
      count++;
    }

    if (count == 0) {
      rg_gui_alert(_("Cheats"),
                   _("No cheats active. Use 'Load Cheats' or 'Add Cheats'."));
      break;
    }

    choices[count++] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t sel_arg = rg_gui_dialog(_("Cheats Menu"), choices, last_cheat_sel);

    if (sel_arg == RG_DIALOG_CANCELLED)
      break;
  }
}

static void handle_add_cheat_menu(void) {
  char *code = rg_gui_input_str(_("Add Cheat"), _("Enter Code (GameShark)"), "");
  if (code) {
    char *name = rg_gui_input_str(_("Add Cheat"), _("Enter Description"), "");
    if (name) {
      apply_cheat_code(code, name, true);
      rg_gui_alert(_("Add Cheat"), _("Cheat added successfully."));
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
      uint16_t a;
      uint8_t v;
      bool s;
      char *full_name = NULL;
      if (!gb_cheat_get(i, &full_name, &a, &v, &s))
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
      rg_gui_alert(_("Delete Cheats"), _("No cheats to delete."));
      break;
    }

    choices[count++] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t sel_arg = rg_gui_dialog(_("Select Cheat to Delete"), choices, 0);

    if (sel_arg == RG_DIALOG_CANCELLED)
      break;

    if (sel_arg >= 0 && sel_arg < 30) {
      gb_cheat_del((uint32_t)sel_arg);
    }
  }
}

static rg_gui_event_t handle_load_cheats_cb(rg_gui_option_t *opt,
                                            rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    load_cheats();
    rg_gui_alert(_("Load Cheats"), _("Cheats loaded from SD Card."));
    return RG_DIALOG_VOID;
  }
  return RG_DIALOG_VOID;
}

static rg_gui_event_t handle_save_cheats_cb(rg_gui_option_t *opt,
                                            rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    save_cheats();
    rg_gui_alert(_("Save Cheats"), _("Cheats saved to SD Card."));
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
  if (event == RG_DIALOG_ENTER) {
    handle_add_cheat_menu();
    return RG_DIALOG_VOID;
  }
  return RG_DIALOG_VOID;
}

static rg_gui_event_t handle_delete_cheat_menu_cb(rg_gui_option_t *opt,
                                                  rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    handle_delete_cheat_menu();
    return RG_DIALOG_VOID;
  }
  return RG_DIALOG_VOID;
}

static rg_gui_event_t handle_cheat_menu_cb(rg_gui_option_t *opt,
                                           rg_gui_event_t event) {
  if (event == RG_DIALOG_ENTER) {
    const rg_gui_option_t choices[] = {
        {0, _("Active Cheats"), ">", RG_DIALOG_FLAG_NORMAL, &handle_cheat_list_cb},
        {0, _("Add Cheat"), "-", RG_DIALOG_FLAG_NORMAL, &handle_add_cheat_menu_cb},
        {0, _("Delete Cheat"), "-", RG_DIALOG_FLAG_NORMAL, &handle_delete_cheat_menu_cb},
        {0, _("Load Cheats"), "-", RG_DIALOG_FLAG_NORMAL, &handle_load_cheats_cb},
        {0, _("Save Cheats"), "-", RG_DIALOG_FLAG_NORMAL, &handle_save_cheats_cb},
        RG_DIALOG_END};

    rg_gui_dialog(_("Cheats"), choices, 0);
    save_cheats();
  }
  return RG_DIALOG_VOID;
}

static void audio_callback(void *buffer, size_t length) {
  audio_msg_t *msg;
  if (xQueueReceive(audio_queue_empty, &msg, 0) == pdTRUE) {
    size_t count = length >> 1; // Stereo S16 samples
    if (count > AUDIO_BUFFER_SIZE) count = AUDIO_BUFFER_SIZE;
    memcpy(msg->frames, buffer, count * 4); // 2 channels * 2 bytes
    msg->count = count;
    xQueueSend(audio_queue_full, &msg, portMAX_DELAY);
  }
}

static void options_handler(rg_gui_option_t *dest) {
  const char *title = rg_gui_get_dialog_title();
  bool is_netplay_menu = (title && strcmp(title, _("Netplay")) == 0);

  if (!is_netplay_menu) {
    *dest++ = (rg_gui_option_t){0, _("Cheats"), ">", RG_DIALOG_FLAG_NORMAL, &handle_cheat_menu_cb};
    *dest++ = (rg_gui_option_t){0, _("Palette"), "-", RG_DIALOG_FLAG_NORMAL, &palette_update_cb};
    *dest++ = (rg_gui_option_t){0, _("RTC config"), "-", RG_DIALOG_FLAG_NORMAL, &rtc_update_cb};
    *dest++ = (rg_gui_option_t){0, _("SRAM autosave"), "-", RG_DIALOG_FLAG_NORMAL, &sram_autosave_cb};
    *dest++ = (rg_gui_option_t){0, _("Enable BIOS"), "-", RG_DIALOG_FLAG_NORMAL, &enable_bios_cb};
  } else {
#ifdef RG_ENABLE_NETPLAY
    *dest++ = (rg_gui_option_t){0, _("Netplay quick start"), "-", RG_DIALOG_FLAG_NORMAL, &netplay_start_cb};
#endif
  }
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

  app = rg_system_init(AUDIO_SAMPLE_RATE, &handlers, NULL);
  rg_system_set_overclock(1);

  gb_cheat_init();
  load_cheats();

  // Initialize Async Audio Pool on Core 0
  audio_queue_full = xQueueCreate(1, sizeof(audio_msg_t *));
  audio_queue_empty = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(audio_msg_t *));
  for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
    audio_msg_t *msg = &audio_pool[i];
    xQueueSend(audio_queue_empty, &msg, 0);
  }
  xTaskCreatePinnedToCore(&audio_task, "audio_task", 2048, NULL, 5, NULL, 0);

  updates[0] = rg_surface_create(GB_WIDTH, GB_HEIGHT, RG_PIXEL_565_BE, MEM_SLOW);
  updates[1] = rg_surface_create(GB_WIDTH, GB_HEIGHT, RG_PIXEL_565_BE, MEM_SLOW);
  currentUpdate = updates[0];

  useSystemTime = (bool)rg_settings_get_number(NS_APP, SETTING_SYSTIME, 1);
  loadBIOSFile = (bool)rg_settings_get_number(NS_APP, SETTING_LOADBIOS, 0);
  autoSaveSRAM = (int)rg_settings_get_number(NS_APP, SETTING_SAVESRAM, 0);
  sramFile = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);

  if (!rg_storage_mkdir(rg_dirname(sramFile)))
    RG_LOGE("Unable to create SRAM folder...");

  if (gnuboy_init(app->sampleRate, GB_AUDIO_STEREO_S16, GB_PIXEL_565_BE, &video_callback, &audio_callback) < 0)
    RG_PANIC("Emulator init failed!");

#ifdef RG_ENABLE_NETPLAY
  gnuboy_set_serial_callback(&gb_link_serial_exchange);
  gnuboy_set_serial_poll_callback(&gb_link_serial_poll);
#endif

  gnuboy_set_framebuffer(currentUpdate->data);
  // Using a smaller buffer size (1 frame = 735 samples) for better latency and clarity
  const size_t gnuboy_buffer_len = 800; 
  void *sound_buffer = rg_alloc(gnuboy_buffer_len * 4, MEM_FAST);
  gnuboy_set_soundbuffer(sound_buffer, gnuboy_buffer_len);

  void *rom_data = NULL;
  size_t rom_size = 0;

  if (rg_extension_match(app->romPath, "zip")) {
    rg_storage_unzip_file(app->romPath, NULL, &rom_data, &rom_size, RG_FILE_ALIGN_16KB);
  }

  if (!rom_data) {
    rg_stat_t st = rg_storage_stat(app->romPath);
    if (st.size > 0) {
      rom_size = st.size;
      rom_data = rg_alloc(rom_size, MEM_SLOW);
      if (rom_data) {
        if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size, RG_FILE_USER_BUFFER)) {
          free(rom_data);
          rom_data = NULL;
        }
      }
    }
  }

  if (!rom_data || gnuboy_load_rom(rom_data, rom_size) < 0) {
    RG_PANIC("ROM Loading failed!");
  }

  if (loadBIOSFile) {
    if (gnuboy_get_hwtype() == GB_HW_CGB)
      gnuboy_load_bios_file(RG_BASE_PATH_BIOS "/gbc_bios.bin");
    else if (gnuboy_get_hwtype() == GB_HW_DMG)
      gnuboy_load_bios_file(RG_BASE_PATH_BIOS "/gb_bios.bin");
  }

  gnuboy_set_palette(rg_settings_get_number(NS_APP, SETTING_PALETTE, GB_PALETTE_DMG));
  gnuboy_reset(true);

  if (app->bootFlags & RG_BOOT_RESUME)
    rg_emu_load_state(app->saveSlot);
  else
    gnuboy_load_sram(sramFile);

  update_rtc_time();

  static uint32_t joystick_old = 0;
  static bool menu_cancelled = false;
  static bool menu_pressed = false;
  static bool turbo_a_toggled = false;
  static bool turbo_b_toggled = false;
  static int turbo_counter = 0;

  while (!rg_system_exit_called()) {
    uint32_t joystick = rg_input_read_gamepad();
    uint32_t joystick_down = joystick & ~joystick_old;
    turbo_counter++;

    if (joystick & RG_KEY_MENU) {
      if (joystick_down & RG_KEY_A) turbo_a_toggled = !turbo_a_toggled;
      if (joystick_down & RG_KEY_B) turbo_b_toggled = !turbo_b_toggled;
      if (joystick & ~RG_KEY_MENU) menu_cancelled = true;
      menu_pressed = true;
    } else {
      if (joystick_old & RG_KEY_MENU) {
        if (!menu_cancelled) {
          if (gnuboy_sram_dirty()) gnuboy_save_sram(sramFile, false);
#ifdef RG_ENABLE_NETPLAY
          rg_netplay_send_pause(true);
#endif
          rg_gui_game_menu();
#ifdef RG_ENABLE_NETPLAY
          rg_netplay_send_pause(false);
#endif
        }
        menu_cancelled = false;
      }
      menu_pressed = false;
    }

    if (joystick & RG_KEY_OPTION) {
#ifdef RG_ENABLE_NETPLAY
      rg_netplay_send_pause(true);
#endif
      rg_gui_options_menu();
#ifdef RG_ENABLE_NETPLAY
      rg_netplay_send_pause(false);
#endif
    }

    if (!menu_pressed) {
      int pad = 0;
      if (joystick & RG_KEY_UP) pad |= GB_PAD_UP;
      if (joystick & RG_KEY_RIGHT) pad |= GB_PAD_RIGHT;
      if (joystick & RG_KEY_DOWN) pad |= GB_PAD_DOWN;
      if (joystick & RG_KEY_LEFT) pad |= GB_PAD_LEFT;
      if (joystick & RG_KEY_SELECT) pad |= GB_PAD_SELECT;
      if (joystick & RG_KEY_START) pad |= GB_PAD_START;
      if (joystick & RG_KEY_A) {
        if (!turbo_a_toggled || (turbo_counter & 4)) pad |= GB_PAD_A;
      }
      if (joystick & RG_KEY_B) {
        if (!turbo_b_toggled || (turbo_counter & 4)) pad |= GB_PAD_B;
      }
      gnuboy_set_pad(pad);
    }
    joystick_old = joystick;

    int64_t startTime = rg_system_timer();
    bool drawFrame = !skipFrames;

    video_time = audio_time = 0;

    if (drawFrame) {
      currentUpdate = updates[currentUpdate == updates[0]];
      gnuboy_set_framebuffer(currentUpdate->data);
    }
    gnuboy_run(drawFrame);

    if (autoSaveSRAM > 0) {
      if (autoSaveSRAM_Timer <= 0) {
        if (gnuboy_sram_dirty()) autoSaveSRAM_Timer = autoSaveSRAM * 60;
      } else if (--autoSaveSRAM_Timer == 0) {
        gnuboy_save_sram(sramFile, true);
      }
    }

    rg_system_tick(rg_system_timer() - startTime);

    if (skipFrames == 0) {
      int elapsed = rg_system_timer() - startTime;
      if (app->frameskip > 0) skipFrames = app->frameskip;
      else if (elapsed > app->frameTime + 1500) skipFrames = 1;
      else if (drawFrame && slowFrame) skipFrames = 1;
    } else if (skipFrames > 0) {
      skipFrames--;
    }

    rg_system_sync_frame(startTime);
  }

  // Signal audio task to shutdown
  audio_msg_t *shutdown_msg = (audio_msg_t *)-1;
  xQueueSend(audio_queue_full, &shutdown_msg, 0);

  if (gnuboy_sram_dirty()) gnuboy_save_sram(sramFile, false);
  
  gnuboy_free_rom();
  
  if (rom_data) free(rom_data);
  if (sound_buffer) free(sound_buffer);
  if (updates[0]) rg_surface_free(updates[0]);
  if (updates[1]) rg_surface_free(updates[1]);

  if (audio_queue_full) vQueueDelete(audio_queue_full);
  if (audio_queue_empty) vQueueDelete(audio_queue_empty);

  rom_data = NULL;
  sound_buffer = NULL;
  updates[0] = updates[1] = NULL;
}
