#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs_dev.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"

#include "rg_system.h"
#include "rg_network.h"
#ifdef RG_ENABLE_NETPLAY
#include "rg_netplay.h"
#endif
#include "quake_main.h"

static const char TAG[] = "main";

#if defined(CONFIG_IDF_TARGET_ESP32)
#define ESP32_QUAKE_TASK_STACK_SIZE (64 * 1024)
static DRAM_ATTR uint8_t quake_task_stack[ESP32_QUAKE_TASK_STACK_SIZE] __attribute__((aligned(16)));
#else
#define ESP32_QUAKE_TASK_STACK_SIZE (256 * 1024)
static EXT_RAM_BSS_ATTR uint8_t quake_task_stack[ESP32_QUAKE_TASK_STACK_SIZE] __attribute__((aligned(16)));
#endif
static StaticTask_t quake_task_internal;
static volatile TaskHandle_t quake_task;

static rg_app_t *app;

// From sys_esp32.c
void Sys_Quit(void);

void event_handler(int event, void *data)
{
    if (event == RG_EVENT_SHUTDOWN) {
        ESP_LOGI(TAG, "Shutdown event received");
        Sys_Quit();
    }
}

void user_task(void *arg)
{
    ESP_LOGI(TAG, "Quake task started");
    
    const char *basedir = RG_BASE_PATH_ROMS "/quake";
    const char *pakPath = app->romPath; 

    // If no rom path provided, default to pak0.pak in basedir/id1
    if (!pakPath || strlen(pakPath) == 0) {
        pakPath = RG_BASE_PATH_ROMS "/quake/id1/pak0.pak";
    }

    quake_main(basedir, pakPath, 0, NULL);

    quake_task = NULL;
    vTaskDelete(NULL);
}

void app_main(void)
{
    rg_handlers_t handlers = {
        .event = event_handler,
    };

    app = rg_system_init(11025, &handlers, NULL);

#if defined(CONFIG_IDF_TARGET_ESP32S3)
    rg_system_set_overclock(1); // Level 1 is usually enough for S3
#endif

    // Disable WiFi and Netplay to save memory and improve stability
#ifdef RG_ENABLE_NETWORKING
    rg_network_wifi_stop();
#endif
#ifdef RG_ENABLE_NETPLAY
    rg_netplay_stop();
#endif

    rg_stats_t stats = rg_system_get_stats();
    ESP_LOGI(TAG, "Memory: Internal free %d KB, External free %d KB", 
             stats.freeMemoryInt / 1024, stats.freeMemoryExt / 1024);

    rg_storage_mkdir(RG_BASE_PATH_CONFIG "/quake");
    rg_storage_mkdir(RG_BASE_PATH_SAVES "/quake");

    if ((quake_task = xTaskCreateStaticPinnedToCore(user_task, "quake_task", ESP32_QUAKE_TASK_STACK_SIZE, NULL, RG_TASK_PRIORITY_5, quake_task_stack, &quake_task_internal, 1)) == NULL)
    {
        ESP_LOGE(TAG, "failed to start quake task");
        rg_system_exit();
    }

    while (quake_task != NULL)
    {
        rg_task_delay(100);
    }

    ESP_LOGI(TAG, "Quake task finished, exiting...");
    rg_system_exit();
}
