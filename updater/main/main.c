#define RG_LOG_TAG "updater"
#include <esp_err.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
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

static void update_partition(FILE *f, const esp_partition_t *dest,
                             uint32_t src_offset, uint32_t src_size) {
  if (src_size == 0)
    return;

  RG_LOGI("Updating partition '%s' (0x%x bytes) from offset 0x%x", dest->label,
          src_size, src_offset);
  rg_gui_draw_message("Erasing %s...", dest->label);

  // We only erase the size needed, or the whole partition?
  // Usually it's better to erase the whole partition if it's an app.
  if (esp_partition_erase_range(dest, 0, dest->size) != ESP_OK) {
    RG_LOGE("Erase failed for %s", dest->label);
    return;
  }

  rg_gui_draw_message("Writing %s...", dest->label);

  uint8_t *buffer = malloc(4096);
  uint32_t written = 0;
  fseek(f, src_offset, SEEK_SET);

  while (written < src_size) {
    uint32_t to_read =
        (src_size - written) > 4096 ? 4096 : (src_size - written);
    if (fread(buffer, 1, to_read, f) != to_read)
      break;

    if (esp_partition_write(dest, written, buffer, to_read) != ESP_OK) {
      RG_LOGE("Write failed for %s at offset 0x%x", dest->label, written);
      break;
    }

    written += to_read;
    // rg_gui_draw_message("Writing %s: %d%%", dest->label, (int)(written * 100
    // / src_size));
  }

  free(buffer);
}

static void do_update(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    rg_gui_alert("Update failed", "Could not open file.");
    return;
  }

  fseek(f, 0, SEEK_END);
  size_t file_size = ftell(f);

  if (file_size < 0x10000) {
    rg_gui_alert("Update failed", "File too small.");
    fclose(f);
    return;
  }

  // Check footer
  fseek(f, file_size - 256, SEEK_SET);
  char footer[256];
  fread(footer, 1, 256, f);

  if (memcmp(footer, "RG_IMG_0", 8) != 0) {
    if (!rg_gui_confirm("Warning", "Invalid footer. Proceed anyway?", false)) {
      fclose(f);
      return;
    }
  }

  if (!rg_gui_confirm("Confirm", "Flash firmware now?\nDevice will reboot.",
                      true)) {
    fclose(f);
    return;
  }

  // Read partition table from .img at 0x8000
  fseek(f, 0x8000, SEEK_SET);
  rg_partition_info_t info[32]; // Max 32 partitions
  int count = fread(info, sizeof(rg_partition_info_t), 32, f);

  rg_gui_draw_message("Starting update...");

  for (int i = 0; i < count; i++) {
    if (info[i].magic != 0x50AA)
      break; // 0xAA 0x50 in little-endian

    char label[17];
    memcpy(label, info[i].label, 16);
    label[16] = 0;

    // Skip dangerous or unnecessary partitions
    if (strcmp(label, "nvs") == 0 || strcmp(label, "otadata") == 0 ||
        strcmp(label, "phy_init") == 0)
      continue;

    // Skip current application (updater)
    if (strcmp(label, "updater") == 0)
      continue;

    const esp_partition_t *dest =
        esp_partition_find_first(info[i].type, info[i].subtype, label);
    if (dest) {
      update_partition(f, dest, info[i].offset, info[i].size);
    } else {
      RG_LOGW("Partition '%s' not found on device, skipping.", label);
    }
  }

  fclose(f);

  rg_gui_alert("Success", "Update complete! Rebooting...");
  rg_system_restart();
}

void app_main(void) {
  rg_app_t *app = rg_system_init(11025, NULL, NULL);

  RG_LOGI("Updater started. BootArgs: %s", app->bootArgs ?: "None");

  if (app->bootArgs && strlen(app->bootArgs) > 0) {
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

  rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
}
