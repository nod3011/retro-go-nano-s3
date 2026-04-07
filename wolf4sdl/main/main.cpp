/*
 * WOLF4SDL FOR RETRO-GO
 * 
 * To build for Spear of Destiny (SOD) instead of Wolfenstein 3D:
 * 1. Open "components/wolf4sdl/wl_main/version.h"
 * 2. Uncomment the line: #define SPEAR
 */

#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSIONALREADYCHOSEN

#include "wl_def.h"
#include "wl_main.h"

#define AUDIO_SAMPLE_RATE 22050

#if RG_SCREEN_PIXEL_FORMAT == 0
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_BE
#else
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_LE
#endif

static rg_surface_t *update;
static rg_app_t *app;

// Standard Retro-Go button mapping
typedef struct {
    int rg_key;
    SDL_Scancode scancode;
    SDL_Keycode keycode;
} rg_bind_t;

static rg_bind_t binds[] = {
    {RG_KEY_UP, SDL_SCANCODE_UP, SDLK_UP},
    {RG_KEY_DOWN, SDL_SCANCODE_DOWN, SDLK_DOWN},
    {RG_KEY_LEFT, SDL_SCANCODE_LEFT, SDLK_LEFT},
    {RG_KEY_RIGHT, SDL_SCANCODE_RIGHT, SDLK_RIGHT},
    {RG_KEY_A, SDL_SCANCODE_LCTRL, SDLK_LCTRL},   // Fire
    {RG_KEY_B, SDL_SCANCODE_LALT, SDLK_LALT},     // Strafe
    {RG_KEY_X, SDL_SCANCODE_SPACE, SDLK_SPACE},   // Open/Use
    {RG_KEY_Y, SDL_SCANCODE_Y, SDLK_y},           // Weapon Cycle
    {RG_KEY_L, SDL_SCANCODE_COMMA, SDLK_COMMA},   // Strafe left
    {RG_KEY_R, SDL_SCANCODE_PERIOD, SDLK_PERIOD}, // Strafe right
    {RG_KEY_START, SDL_SCANCODE_SPACE, SDLK_SPACE}, // Open/Use
    {RG_KEY_OPTION, SDL_SCANCODE_UNKNOWN, (SDLKey)0}, // Retro-Go System
    {RG_KEY_MENU, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE}, // Menu
    {0, SDL_SCANCODE_UNKNOWN, (SDLKey)0}
};

static uint32_t last_input = 0;
static bool is_run_toggle_on = false;
static bool is_select_pressed = false;
static bool is_select_used = false;
static SDL_Keycode pending_keyup = (SDLKey)0;
static SDL_Scancode pending_scancode = SDL_SCANCODE_UNKNOWN;
static uint32_t keyup_at = 0;

extern "C" int SDL_RG_PollEvent(SDL_Event *event)
{
    // Handle pending KEYUP events from synthesized combinations with a small delay
    if (pending_keyup != (SDLKey)0 && SDL_GetTicks() >= keyup_at) {
        event->type = SDL_KEYUP;
        event->key.keysym.scancode = pending_scancode;
        event->key.keysym.sym = pending_keyup;
        event->key.state = SDL_RELEASED;
        pending_keyup = (SDLKey)0;
        pending_scancode = SDL_SCANCODE_UNKNOWN;
        return 1;
    }

    uint32_t current_input = rg_input_read_gamepad();
    uint32_t changed = current_input ^ last_input;
    uint32_t pressed = current_input & changed;

    // 1. Handle Select button state transitions
    if (changed & RG_KEY_SELECT) {
        if (pressed & RG_KEY_SELECT) {
            is_select_pressed = true;
            is_select_used = false;
            last_input |= RG_KEY_SELECT; // Update state
        } else {
            // Select Released
            is_select_pressed = false;
            bool was_used = is_select_used;
            last_input &= ~RG_KEY_SELECT; // Ensure last_input is updated
            
            if (!was_used) {
                // Tapped Select -> Next Weapon (Y)
                event->type = SDL_KEYDOWN;
                event->key.keysym.scancode = SDL_SCANCODE_Y;
                event->key.keysym.sym = SDLK_y;
                event->key.state = SDL_PRESSED;
                pending_keyup = SDLK_y; // Queue KEYUP for next poll
                pending_scancode = SDL_SCANCODE_Y;
                keyup_at = SDL_GetTicks() + 50;
                return 1;
            }
            return 0; // Select was used for a combination, just consume the release
        }
    }

    // 2. Handle Select combinations
    if (is_select_pressed) {
        if (pressed & RG_KEY_UP) { // Toggle Run ON
            is_run_toggle_on = true;
            is_select_used = true;
            last_input |= RG_KEY_UP; // Update state
            event->type = SDL_KEYDOWN;
            event->key.keysym.scancode = SDL_SCANCODE_LSHIFT;
            event->key.keysym.sym = SDLK_LSHIFT;
            event->key.state = SDL_PRESSED;
            return 1;
        }
        if (pressed & RG_KEY_DOWN) { // Toggle Run OFF
            is_run_toggle_on = false;
            is_select_used = true;
            last_input |= RG_KEY_DOWN; // Update state
            event->type = SDL_KEYUP;
            event->key.keysym.scancode = SDL_SCANCODE_LSHIFT;
            event->key.keysym.sym = SDLK_LSHIFT;
            event->key.state = SDL_RELEASED;
            return 1;
        }
    }

    // 3. Handle standard binds
    if (changed) {
        for (int i = 0; binds[i].rg_key; i++) {
            // When Select is held, we ignore D-Pad for starting new movement (KEYDOWN)
            // But we ALWAYS allow releases (KEYUP) to prevent stuck keys.
            if (is_select_pressed && (current_input & binds[i].rg_key) && 
                (binds[i].rg_key & (RG_KEY_UP | RG_KEY_DOWN | RG_KEY_LEFT | RG_KEY_RIGHT))) {
                last_input ^= binds[i].rg_key; // Keep state in sync
                continue; // Check next key
            }
            
            if (changed & binds[i].rg_key) {
                event->type = (current_input & binds[i].rg_key) ? SDL_KEYDOWN : SDL_KEYUP;
                event->key.keysym.scancode = binds[i].scancode;
                event->key.keysym.sym = binds[i].keycode;
                event->key.state = (current_input & binds[i].rg_key) ? SDL_PRESSED : SDL_RELEASED;
                
                last_input ^= binds[i].rg_key;
                return 1;
            }
        }
    }
    return 0;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(update, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    rg_gui_alert("Not implemented", "Please use the in-game menu");
    return false;
}

static bool load_state_handler(const char *filename)
{
    rg_gui_alert("Not implemented", "Please use the in-game menu");
    return false;
}

static bool reset_handler(bool hard)
{
    return false;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(update, 0);
    }
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

static bool parse_w3d(const char *path, char *folder, size_t folder_sz)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char *p = strstr(line, "folder=");
        if (p)
        {
            p += 7;
            if (*p == '"') p++;
            size_t i = 0;
            while (*p && *p != '"' && *p != '\n' && *p != '\r' && i < folder_sz - 1)
                folder[i++] = *p++;
            folder[i] = 0;
        }
    }
    fclose(f);
    return true;
}

extern int wolf_main(int argc, char *argv[]);

extern "C" void app_main()
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .memRead = NULL,
        .memWrite = NULL,
        .options = &options_handler,
        .about = NULL,
    };

    app = rg_system_init(AUDIO_SAMPLE_RATE, &handlers, NULL);
    rg_system_set_tick_rate(70);

    // Constant internal resolution for consistency.
    int width = 320;
    int height = 240;

    // Force display to stretch and fill the screen (ignoring aspect ratio).
    rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);

    update = rg_surface_create(width, height, FB_PIXEL_FORMAT, MEM_FAST);

    extern void SDL_RG_SetSurface(rg_surface_t *surf);
    SDL_RG_SetSurface(update);

    char current_datadir[512] = {0};
    const char *base_path = RG_BASE_PATH_ROMS "/wolf3d/";

    const char *romPath = app->romPath;
    if (romPath && romPath[0])
    {
        if (rg_extension_match(romPath, "w3d"))
        {
            char folder[128] = {0};
            if (parse_w3d(romPath, folder, sizeof(folder)))
            {
                snprintf(current_datadir, sizeof(current_datadir), "%s%s/", base_path, folder);
            }
        }
        else if (rg_storage_exists(romPath))
        {
            // If it's a direct file (wl6, wl1, etc)
            const char *lastSlash = strrchr(romPath, '/');
            if (lastSlash)
            {
                size_t len = lastSlash - romPath + 1;
                if (len < sizeof(current_datadir))
                {
                    strncpy(current_datadir, romPath, len);
                    current_datadir[len] = 0;
                }
            }
        }
    }

    // If still no path, try a "data" default if it exists, otherwise exit
    if (current_datadir[0] == 0)
    {
        snprintf(current_datadir, sizeof(current_datadir), "%sdata/", base_path);
        
        if (!rg_storage_exists(current_datadir))
        {
            rg_gui_alert("No ROM selected", "Please launch a .w3d file from the menu.");
            rg_system_exit();
            return;
        }
    }

    RG_LOGI("Using data directory: %s\n", current_datadir);

    // Prepare arguments for Wolf4SDL
    char arg0[] = "wolf4sdl";
    char arg1[] = "--res";
    char arg2[] = "320";
    char arg3[] = "240";
    char arg4[] = "--samplerate";
    char arg5[] = "11025";
    char arg6[] = "--configdir";
    char arg7[256];
    strcpy(arg7, RG_BASE_PATH_CONFIG "/wolf3d/");
    char arg8[] = "--savedir";
    char arg9[256];
    strcpy(arg9, RG_BASE_PATH_SAVES "/wolf3d/");
    char arg10[] = "--datadir";
    char arg11[350];
    strcpy(arg11, current_datadir);
    
    char *argv[] = { arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, NULL };
    int argc = 12;

    // Call Wolf4SDL main
    wolf_main(argc, argv);

    rg_system_exit();
}

