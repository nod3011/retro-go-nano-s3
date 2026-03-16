/* ESP32-S3 Dual-Core Processing for GBSP
 *
 * Copyright (C) 2024 Retro-Go Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef ESP32S3_DUALCORE_H
#define ESP32S3_DUALCORE_H

#ifdef ESP32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp32s3_opt.h"

// Core assignment
#define ESP32S3_CPU_CORE    0  // CPU emulation on core 0
#define ESP32S3_VIDEO_CORE   1  // Video rendering on core 1

// Task stack sizes
#define ESP32S3_CPU_STACK_SIZE    8192
#define ESP32S3_VIDEO_STACK_SIZE 4096

// Message types for inter-core communication
typedef enum {
    MSG_FRAME_START = 0,
    MSG_SCANLINE_READY,
    MSG_FRAME_COMPLETE,
    MSG_PERFORMANCE_UPDATE,
    MSG_SHUTDOWN
} core_message_type_t;

// Scanline data structure for video processing
typedef struct {
    u16 scanline_data[240];     // One scanline of pixel data
    u32 scanline_number;         // Which scanline (0-159)
    u32 frame_number;           // Frame counter
    u32 render_flags;           // Rendering flags/effects
    u32 cycles_used;            // CPU cycles used for this scanline
} scanline_data_t;

// Frame data structure
typedef struct {
    scanline_data_t scanlines[160];  // All scanlines for one frame
    u32 frame_number;               // Frame identifier
    u32 total_cycles;               // Total CPU cycles for frame
    u32 start_time;                 // Frame start timestamp
    u32 video_mode;                 // Current video mode
    bool skip_frame;                // Whether to skip rendering this frame
} frame_data_t;

// Inter-core message structure
typedef struct {
    core_message_type_t type;
    void* data;
    u32 timestamp;
} core_message_t;

// Dual-core state structure
typedef struct {
    TaskHandle_t cpu_task;
    TaskHandle_t video_task;
    
    // Communication queues
    QueueHandle_t cpu_to_video_queue;
    QueueHandle_t video_to_cpu_queue;
    
    // Synchronization
    SemaphoreHandle_t frame_mutex;
    SemaphoreHandle_t scanline_ready_sem;
    
    // Frame buffers (double buffering)
    frame_data_t frame_buffers[2];
    u32 current_write_buffer;
    u32 current_read_buffer;
    
    // Performance tracking
    u32 frames_processed;
    u32 frames_dropped;
    u32 avg_frame_time;
    
    // State flags
    bool video_task_running;
    bool cpu_task_running;
    bool shutdown_requested;
} esp32s3_dualcore_state_t;

// Function declarations
void esp32s3_dualcore_init(void);
void esp32s3_dualcore_shutdown(void);
void esp32s3_dualcore_start_frame(void);
void esp32s3_dualcore_end_frame(void);
void esp32s3_dualcore_submit_scanline(u32 scanline_num, u16* data, u32 flags);
bool esp32s3_dualcore_wait_for_frame(void);

// Task functions
void esp32s3_cpu_task(void* pvParameters);
void esp32s3_video_task(void* pvParameters);

// Internal functions
static void esp32s3_send_message(core_message_type_t type, void* data);
static bool esp32s3_receive_message(core_message_type_t* type, void** data, u32 timeout);
static void esp32s3_swap_buffers(void);

#endif // ESP32

#endif // ESP32S3_DUALCORE_H
