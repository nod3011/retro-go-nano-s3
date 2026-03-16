/* ESP32-S3 Dual-Core Processing Implementation for GBSP
 *
 * Copyright (C) 2024 Retro-Go Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "esp32s3_dualcore.h"
#include "common.h"
#include "video.h"
#include "main.h"

#ifdef ESP32

// Global dual-core state
static esp32s3_dualcore_state_t dualcore_state = {0};

void esp32s3_dualcore_init(void) {
    // Create communication queues
    dualcore_state.cpu_to_video_queue = xQueueCreate(32, sizeof(core_message_t));
    dualcore_state.video_to_cpu_queue = xQueueCreate(16, sizeof(core_message_t));
    
    // Create synchronization primitives
    dualcore_state.frame_mutex = xSemaphoreCreateMutex();
    dualcore_state.scanline_ready_sem = xSemaphoreCreateCounting(160, 0);
    
    // Initialize frame buffers
    memset(dualcore_state.frame_buffers, 0, sizeof(dualcore_state.frame_buffers));
    dualcore_state.current_write_buffer = 0;
    dualcore_state.current_read_buffer = 1;
    
    // Initialize performance counters
    dualcore_state.frames_processed = 0;
    dualcore_state.frames_dropped = 0;
    dualcore_state.avg_frame_time = 0;
    
    // Reset state flags
    dualcore_state.video_task_running = false;
    dualcore_state.cpu_task_running = false;
    dualcore_state.shutdown_requested = false;
    
    // Create tasks on different cores
    xTaskCreatePinnedToCore(esp32s3_cpu_task, "gba_cpu", 
                          ESP32S3_CPU_STACK_SIZE, NULL, 
                          ESP32S3_CPU_TASK_PRIORITY, 
                          &dualcore_state.cpu_task, ESP32S3_CPU_CORE);
    
    xTaskCreatePinnedToCore(esp32s3_video_task, "gba_video", 
                          ESP32S3_VIDEO_STACK_SIZE, NULL, 
                          ESP32S3_VIDEO_TASK_PRIORITY, 
                          &dualcore_state.video_task, ESP32S3_VIDEO_CORE);
    
    // Wait for tasks to initialize
    vTaskDelay(pdMS_TO_TICKS(100));
}

void esp32s3_dualcore_shutdown(void) {
    dualcore_state.shutdown_requested = true;
    
    // Send shutdown messages
    esp32s3_send_message(MSG_SHUTDOWN, NULL);
    
    // Wait for tasks to finish
    if (dualcore_state.cpu_task) {
        vTaskDelete(dualcore_state.cpu_task);
    }
    if (dualcore_state.video_task) {
        vTaskDelete(dualcore_state.video_task);
    }
    
    // Clean up queues and semaphores
    if (dualcore_state.cpu_to_video_queue) {
        vQueueDelete(dualcore_state.cpu_to_video_queue);
    }
    if (dualcore_state.video_to_cpu_queue) {
        vQueueDelete(dualcore_state.video_to_cpu_queue);
    }
    if (dualcore_state.frame_mutex) {
        vSemaphoreDelete(dualcore_state.frame_mutex);
    }
    if (dualcore_state.scanline_ready_sem) {
        vSemaphoreDelete(dualcore_state.scanline_ready_sem);
    }
}

void esp32s3_dualcore_start_frame(void) {
    frame_data_t* frame = &dualcore_state.frame_buffers[dualcore_state.current_write_buffer];
    
    // Initialize frame data
    frame->frame_number = dualcore_state.frames_processed;
    frame->start_time = esp32s3_get_cycle_count();
    frame->total_cycles = 0;
    frame->video_mode = read_ioreg(REG_DISPCNT) & 0x07;
    frame->skip_frame = (dualcore_state.frames_dropped > 0);
    
    // Send frame start message to video task
    esp32s3_send_message(MSG_FRAME_START, frame);
}

void esp32s3_dualcore_end_frame(void) {
    frame_data_t* frame = &dualcore_state.frame_buffers[dualcore_state.current_write_buffer];
    
    // Calculate frame statistics
    u32 frame_time = esp32s3_get_cycle_count() - frame->start_time;
    frame->total_cycles = frame_time;
    
    // Update performance tracking
    dualcore_state.frames_processed++;
    if (dualcore_state.avg_frame_time == 0) {
        dualcore_state.avg_frame_time = frame_time;
    } else {
        // Exponential moving average
        dualcore_state.avg_frame_time = (dualcore_state.avg_frame_time * 7 + frame_time) / 8;
    }
    
    // Send frame complete message
    esp32s3_send_message(MSG_FRAME_COMPLETE, frame);
    
    // Swap buffers
    esp32s3_swap_buffers();
    
    // Update ESP32-S3 performance monitor
    esp32s3_update_performance_stats(frame_time);
}

void esp32s3_dualcore_submit_scanline(u32 scanline_num, u16* data, u32 flags) {
    frame_data_t* frame = &dualcore_state.frame_buffers[dualcore_state.current_write_buffer];
    
    if (scanline_num < 160 && data) {
        scanline_data_t* scanline = &frame->scanlines[scanline_num];
        
        // Copy scanline data
        esp32s3_memcpy_aligned(scanline->scanline_data, data, sizeof(scanline->scanline_data));
        scanline->scanline_number = scanline_num;
        scanline->frame_number = frame->frame_number;
        scanline->render_flags = flags;
        scanline->cycles_used = esp32s3_get_cycle_count() - frame->start_time;
        
        // Send scanline ready message
        esp32s3_send_message(MSG_SCANLINE_READY, scanline);
        
        // Release scanline ready semaphore
        xSemaphoreGive(dualcore_state.scanline_ready_sem);
    }
}

bool esp32s3_dualcore_wait_for_frame(void) {
    // Wait for frame completion with timeout
    core_message_type_t msg_type;
    void* msg_data;
    
    if (esp32s3_receive_message(&msg_type, &msg_data, pdMS_TO_TICKS(33))) { // ~30 FPS timeout
        return (msg_type == MSG_FRAME_COMPLETE);
    }
    
    return false; // Timeout
}

void esp32s3_cpu_task(void* pvParameters) {
    dualcore_state.cpu_task_running = true;
    
    while (!dualcore_state.shutdown_requested) {
        // Start new frame
        esp32s3_dualcore_start_frame();
        
        // Run CPU emulation for one frame
        u32 cycles_to_run = 280896; // GBA frame cycles
        u32 result = update_gba(cycles_to_run);
        
        // End frame
        esp32s3_dualcore_end_frame();
        
        // Small delay to prevent starving video task
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    dualcore_state.cpu_task_running = false;
    vTaskDelete(NULL);
}

void esp32s3_video_task(void* pvParameters) {
    dualcore_state.video_task_running = true;
    
    while (!dualcore_state.shutdown_requested) {
        core_message_type_t msg_type;
        void* msg_data;
        
        // Wait for messages from CPU task
        if (esp32s3_receive_message(&msg_type, &msg_data, portMAX_DELAY)) {
            switch (msg_type) {
                case MSG_FRAME_START:
                    // Initialize video rendering for new frame
                    break;
                    
                case MSG_SCANLINE_READY: {
                    scanline_data_t* scanline = (scanline_data_t*)msg_data;
                    if (scanline && scanline->scanline_number < 160) {
                        // Process scanline through video pipeline
                        // This would integrate with existing video rendering code
                        // For now, just mark as processed
                    }
                    break;
                }
                
                case MSG_FRAME_COMPLETE: {
                    frame_data_t* frame = (frame_data_t*)msg_data;
                    if (frame && !frame->skip_frame) {
                        // Composite final frame and send to display
                        // This would integrate with Retro-Go display system
                    }
                    break;
                }
                
                case MSG_PERFORMANCE_UPDATE:
                    // Update performance statistics
                    break;
                    
                case MSG_SHUTDOWN:
                    goto exit_video_task;
                    
                default:
                    break;
            }
        }
    }
    
exit_video_task:
    dualcore_state.video_task_running = false;
    vTaskDelete(NULL);
}

static void esp32s3_send_message(core_message_type_t type, void* data) {
    core_message_t msg = {
        .type = type,
        .data = data,
        .timestamp = esp32s3_get_cycle_count()
    };
    
    // Send to video queue from CPU task
    if (dualcore_state.cpu_to_video_queue) {
        xQueueSend(dualcore_state.cpu_to_video_queue, &msg, 0);
    }
}

static bool esp32s3_receive_message(core_message_type_t* type, void** data, u32 timeout) {
    core_message_t msg;
    
    // Receive from CPU queue in video task
    if (dualcore_state.cpu_to_video_queue && 
        xQueueReceive(dualcore_state.cpu_to_video_queue, &msg, pdMS_TO_TICKS(timeout))) {
        
        *type = msg.type;
        *data = msg.data;
        return true;
    }
    
    return false;
}

static void esp32s3_swap_buffers(void) {
    if (xSemaphoreTake(dualcore_state.frame_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // Swap write and read buffers
        u32 temp = dualcore_state.current_write_buffer;
        dualcore_state.current_write_buffer = dualcore_state.current_read_buffer;
        dualcore_state.current_read_buffer = temp;
        xSemaphoreGive(dualcore_state.frame_mutex);
    }
}

#endif // ESP32
