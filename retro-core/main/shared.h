#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rg_system.h>

#define AUDIO_SAMPLE_RATE   (44100)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 50 + 1)


void launcher_main();
void nes_main();
void pce_main();
void sms_main();
void gw_main();
void lynx_main();
