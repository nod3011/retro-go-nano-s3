#include "quakedef.h"
#include "rg_system.h"

static uint32_t prev_gamepad = 0;
static bool start_combo_active = false;
static bool b_combo_active = false;

// Configuration for directional mapping
static const int normal_keys[] = {K_UPARROW, K_DOWNARROW, K_LEFTARROW, K_RIGHTARROW};
static const int combo_keys[]  = {K_PGDN, K_DEL, ',', '.'};
static const rg_key_t phys_dir[] = {RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT};

void IN_Init(void) {}
void IN_Shutdown(void) {}

void IN_Commands(void)
{
    uint32_t gamepad = rg_input_read_gamepad();
    uint32_t changed = gamepad ^ prev_gamepad;
    uint32_t combo_modifier = RG_KEY_START | RG_KEY_B;
    bool mod_down = (gamepad & combo_modifier) != 0;

    if (changed)
    {
        // 1. Handle Modifier Toggle (Switching modes while holding directions)
        if (changed & combo_modifier)
        {
            // When a modifier is pressed or released, we flush all directions to prevent sticking.
            for (int i = 0; i < 4; i++) {
                bool is_phys_down = (gamepad & phys_dir[i]) != 0;
                if (mod_down) {
                    Key_Event(normal_keys[i], false); // Stop walking
                    Key_Event(combo_keys[i], is_phys_down); // Start looking/strafing if held
                } else {
                    Key_Event(combo_keys[i], false); // Stop looking/strafing
                    Key_Event(normal_keys[i], is_phys_down); // Resume walking if held
                }
            }
            // Reset "used" flags on modifier press
            if ((changed & RG_KEY_START) && (gamepad & RG_KEY_START)) start_combo_active = false;
            if ((changed & RG_KEY_B)     && (gamepad & RG_KEY_B))     b_combo_active = false;
        }

        // 2. Handle Directional Changes
        if (changed & (RG_KEY_UP|RG_KEY_DOWN|RG_KEY_LEFT|RG_KEY_RIGHT))
        {
            for (int i = 0; i < 4; i++) {
                if (changed & phys_dir[i]) {
                    bool is_down = (gamepad & phys_dir[i]) != 0;
                    if (mod_down) {
                        Key_Event(combo_keys[i], is_down);
                        if (is_down) {
                            if (gamepad & RG_KEY_START) start_combo_active = true;
                            if (gamepad & RG_KEY_B)     b_combo_active = true;
                        }
                    } else {
                        Key_Event(normal_keys[i], is_down);
                    }
                }
            }
        }

        // 3. System / Menu Buttons
        if (changed & RG_KEY_MENU) Key_Event(K_ESCAPE, (gamepad & RG_KEY_MENU) != 0);

        if (changed & RG_KEY_SELECT) {
            if (gamepad & RG_KEY_SELECT) Cbuf_AddText("impulse 10\n");
        }

        // A Button: Fire
        if (changed & RG_KEY_A) {
            bool down = (gamepad & RG_KEY_A) != 0;
            if (key_dest == key_game) Key_Event(K_CTRL, down);
            else { Key_Event(K_ENTER, down); if (down) { Key_Event('y', true); Key_Event('y', false); } }
        }

        // B Button: Special Combo + Jump
        if (changed & RG_KEY_B) {
            if (!(gamepad & RG_KEY_B)) { // Release
                if (!b_combo_active && key_dest == key_game) {
                    Key_Event(K_SPACE, true); Key_Event(K_SPACE, false);
                }
            }
            if (key_dest != key_game) { // Menu Navigation
                bool down = (gamepad & RG_KEY_B) != 0;
                Key_Event(K_ESCAPE, down); if (down) { Key_Event('n', true); Key_Event('n', false); }
            }
        }

        // Modifier Release Logic for Start
        if ((changed & RG_KEY_START) && !(gamepad & RG_KEY_START)) {
            // Start release currently does nothing as requested.
        }

        // Swim / Run
        if (changed & (RG_KEY_X | RG_KEY_OPTION)) Key_Event('c', (gamepad & (RG_KEY_X | RG_KEY_OPTION)) != 0);
        if (changed & RG_KEY_Y) Key_Event(K_SHIFT, (gamepad & RG_KEY_Y) != 0);
    }
    prev_gamepad = gamepad;
    
    if ((gamepad & RG_KEY_START) && (gamepad & RG_KEY_SELECT)) rg_gui_game_menu();
}

void IN_Move(usercmd_t *cmd)
{
    uint32_t gamepad = rg_input_read_gamepad();
    uint32_t combo_modifier = RG_KEY_START | RG_KEY_B;
    
    if (!(gamepad & combo_modifier))
    {
        if (gamepad & RG_KEY_UP)    cmd->forwardmove += 200;
        if (gamepad & RG_KEY_DOWN)  cmd->forwardmove -= 200;
        if (gamepad & RG_KEY_LEFT)  cmd->viewangles[YAW] += 1000;
        if (gamepad & RG_KEY_RIGHT) cmd->viewangles[YAW] -= 1000;
    }
    
    if (gamepad & RG_KEY_L) cmd->sidemove -= 200;
    if (gamepad & RG_KEY_R) cmd->sidemove += 200;
}
