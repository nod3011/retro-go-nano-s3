#define RG_LOG_TAG "updater"
#include <esp_err.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <malloc.h>
#include <rg_system.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint16_t magic;
  uint8_t type;
  uint8_t subtype;
  uint32_t offset;
  uint32_t size;
  char label[16];
  uint32_t flags;
} __attribute__((packed)) rg_partition_info_t;

static bool update_file_validator(const char *path) {
  return rg_extension_match(path, "img fw");
}

static void flash_raw(FILE *f, uint32_t dest_offset, uint32_t src_offset,
                      uint32_t size, const char *label) {
  if (size == 0)
    return;

  RG_LOGI("Flashing '%s' to 0x%x (0x%x bytes)", label, dest_offset, size);
  rg_gui_draw_message(_("Flashing %s..."), label);

  esp_flash_t *flash = esp_flash_default_chip;
  uint32_t aligned_size = (size + 4095) & ~4095;

  if (esp_flash_erase_region(flash, dest_offset, aligned_size) != ESP_OK) {
    RG_LOGE("Erase failed for %s", label);
    return;
  }

  uint8_t *buffer = malloc(4096);
  uint32_t written = 0;
  fseek(f, src_offset, SEEK_SET);

  while (written < size) {
    uint32_t to_read = (size - written) > 4096 ? 4096 : (size - written);
    if (fread(buffer, 1, to_read, f) != to_read)
      break;

    if (esp_flash_write(flash, buffer, dest_offset + written, to_read) !=
        ESP_OK) {
      RG_LOGE("Write failed for %s at 0x%x", label, dest_offset + written);
      break;
    }
    written += to_read;
  }
  free(buffer);
}

static void do_update(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    rg_gui_alert(_("Update failed"), _("Could not open file."));
    return;
  }

  fseek(f, 0, SEEK_END);
  size_t file_size = ftell(f);

  if (file_size < 0x10000) {
    rg_gui_alert(_("Update failed"), _("File too small."));
    fclose(f);
    return;
  }

  // Check footer
  fseek(f, file_size - 256, SEEK_SET);
  char footer[256];
  fread(footer, 1, 256, f);

  if (memcmp(footer, "RG_IMG_0", 8) != 0) {
    if (!rg_gui_confirm(_("Warning"), _("Invalid footer. Proceed anyway?"),
                        false)) {
      fclose(f);
      return;
    }
  }

  if (!rg_gui_confirm(_("Confirm"),
                      _("Flash firmware now?\nDevice will reboot."), true)) {
    fclose(f);
    return;
  }

  // 1. Read the new partition entries for iteration
  fseek(f, 0x8000, SEEK_SET);
  rg_partition_info_t info[32]; // Max 32 partitions
  int count = fread(info, sizeof(rg_partition_info_t), 32, f);
  int part_count = 0;
  for (int i = 0; i < count; i++) {
    if (info[i].magic != 0x50AA)
      break;
    part_count++;
  }

  rg_gui_draw_message(_("Cleaning up..."));

  // 2. Erase obsolete APP partitions (using current table)
  esp_partition_iterator_t it = esp_partition_find(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL) {
    const esp_partition_t *p = esp_partition_get(it);
    bool found = false;
    for (int i = 0; i < part_count; i++) {
      if (strncmp(p->label, info[i].label, 16) == 0) {
        found = true;
        break;
      }
    }
    if (!found && strcmp(p->label, "updater") != 0) {
      RG_LOGI("Erasing obsolete partition '%s'", p->label);
      esp_partition_erase_range(p, 0, p->size);
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);

  // 3. Flash internal apps and components using offsets from new table
  for (int i = 0; i < part_count; i++) {
    char label[17];
    memcpy(label, info[i].label, 16);
    label[16] = 0;

    // Skip system partitions and updater
    if (strcmp(label, "nvs") == 0 || strcmp(label, "otadata") == 0 ||
        strcmp(label, "phy_init") == 0 || strcmp(label, "updater") == 0)
      continue;

    // We use the offset/size from the .img's table entry to write to raw flash
    flash_raw(f, info[i].offset, info[i].offset, info[i].size, label);
  }

  // 4. Finally, synchronize the partition table area (0x8000 - 0x8C00)
  rg_gui_draw_message(_("Updating table..."));
  uint8_t table_buffer[4096];
  memset(table_buffer, 0xFF, 4096);
  fseek(f, 0x8000, SEEK_SET);
  fread(table_buffer, 1, 3072, f); // Read entries + MD5

  esp_flash_erase_region(esp_flash_default_chip, 0x8000, 0x1000);
  esp_flash_write(esp_flash_default_chip, table_buffer, 0x8000, 4096);

  fclose(f);

  rg_gui_alert(_("Success"), _("Update complete! Rebooting..."));
  rg_system_restart();
}

void app_main(void) {
  rg_app_t *app = rg_system_init(11025, NULL, NULL);
  bool launcher_missing = !rg_system_have_app(RG_APP_LAUNCHER);

  RG_LOGI("Updater started. BootArgs: %s, Launcher missing: %d",
          app->bootArgs ?: "None", launcher_missing);

  if (launcher_missing) {
    rg_gui_draw_message(_("Launcher not found!\nEntering recovery..."));
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  if (!launcher_missing && app->bootArgs && strlen(app->bootArgs) > 0) {
    do_update(app->bootArgs);
  } else {
    char path[RG_PATH_MAX] = RG_STORAGE_ROOT "/nano-s3/firmware";
#ifdef RG_UPDATER_DOWNLOAD_LOCATION
    strcpy(path, RG_UPDATER_DOWNLOAD_LOCATION);
#endif
    if (!rg_storage_exists(path)) {
      RG_LOGW("Path %s not found, falling back to root", path);
      strcpy(path, RG_STORAGE_ROOT);
    }
    RG_LOGI("Scanning for firmware in %s", path);
    char *selected = rg_gui_file_picker(_("Select firmware"), path,
                                        update_file_validator, true, false);
    if (selected) {
      do_update(selected);
      free(selected);
    }
  }

  if (!rg_system_have_app(RG_APP_LAUNCHER)) {
    rg_gui_alert(_("Flash Failed"),
                 _("Launcher is still missing.\nPlease try again."));
    rg_system_restart();
  }

  rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
}
