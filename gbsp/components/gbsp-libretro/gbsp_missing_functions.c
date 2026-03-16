/* Missing Functions for GBSP Main Component
 *
 * Copyright (C) 2024 Retro-Go Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "common.h"
#include "video.h"
#include "sound.h"
#include "input.h"
#include "gba_memory.h"
#include "cpu.h"

// Missing video task initialization
void init_video_task(void) {
    // Initialize video rendering system
    // For ESP32-S3, this will be handled by dual-core system
#ifdef ESP32
    // Video task is created in dual-core system
#else
    // Single-core initialization
#endif
}

// libretro_supports_bitmasks is already declared in input.h
// Just provide the implementation here
bool libretro_supports_bitmasks = true;

// gamepak_sticky_bit is already declared as array in gba_memory.h
// execute_cycles is already declared in main.h
// Remove conflicting declarations

// Missing retro_set_input_state with correct signature
void retro_set_input_state(retro_input_state_t cb) {
    // Set input state callback for libretro
    // This is handled by update_input in GBSP
}

// Missing execute_arm function
void execute_arm(u32 cycles) {
    // Execute ARM instructions for given cycles
    // This should call the CPU emulation core
    // Don't call update_gba here as it creates circular dependency
    // Just return for now - the actual execution happens elsewhere
}

// Missing rumble frame reset
void rumble_frame_reset(void) {
    // Reset rumble for the frame
    // GBSP doesn't have rumble support, so this is a no-op
}

// Declare external variables that are needed
extern u16* gba_screen_pixels;

// Ensure gba_screen_pixels is properly exported
u16* get_gba_screen_pixels(void) {
    return gba_screen_pixels;
}
