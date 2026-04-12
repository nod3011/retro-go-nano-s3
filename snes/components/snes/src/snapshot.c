/* This file is part of Snes9x. See LICENSE file. */

#include "apu.h"
#include "cpuexec.h"
#include "display.h"
#include "dma.h"
#include "memmap.h"
#include "ppu.h"
#include "snes9x.h"
#include "soundux.h"
#include "srtc.h"
#include <rg_storage.h>
#include <rg_utils.h>


static const char header[16] = "SNES9X_000000002";

bool S9xSaveState(const char *filename) {
  int chunks = 0;
  FILE *fp = NULL;

  printf("Saving state to: %s\n", filename);

  // Ensure directory exists
  char dirname[RG_PATH_MAX];
  const char *dir = rg_dirname(filename);
  if (dir) {
    strcpy(dirname, dir);
    rg_storage_mkdir(dirname);
  }

  if (!(fp = fopen(filename, "wb"))) {
    printf("Failed to open file for writing: %s\n", filename);
    return false;
  }

  chunks += fwrite(&header, sizeof(header), 1, fp);
  chunks += fwrite(&CPU, sizeof(CPU), 1, fp);
  chunks += fwrite(&ICPU, sizeof(ICPU), 1, fp);
  chunks += fwrite(&PPU, sizeof(PPU), 1, fp);
  chunks += fwrite(&DMA, sizeof(DMA), 1, fp);
  chunks += fwrite(Memory.VRAM, VRAM_SIZE, 1, fp);
  chunks += fwrite(Memory.RAM, RAM_SIZE, 1, fp);
  chunks += fwrite(Memory.SRAM, SRAM_SIZE, 1, fp);
  chunks += fwrite(Memory.FillRAM, FILLRAM_SIZE, 1, fp);
  chunks += fwrite(&APU, sizeof(APU), 1, fp);
  chunks += fwrite(&IAPU, sizeof(IAPU), 1, fp);
  chunks += fwrite(IAPU.RAM, 0x10000, 1, fp);
  chunks += fwrite(&SoundData, sizeof(SoundData), 1, fp);

  printf("Saved chunks = %d / 13\n", chunks);

  fclose(fp);

  if (chunks != 13) {
    printf("Warning: Save state incomplete (%d/13 chunks written)\n", chunks);
  }

  return chunks == 13;
}

bool S9xLoadState(const char *filename) {
  uint8_t buffer[512];
  int chunks = 0;
  FILE *fp = NULL;

  printf("Loading state from: %s\n", filename);

  if (!(fp = fopen(filename, "rb"))) {
    printf("Failed to open file for reading: %s\n", filename);
    return false;
  }

  if (!fread(buffer, 16, 1, fp) ||
      memcmp(header, buffer, sizeof(header)) != 0) {
    printf("Wrong header found or file empty\n");
    goto fail;
  }

  // At this point we can't go back and a failure will corrupt the state anyway
  S9xReset();

  uint8_t *IAPU_RAM = IAPU.RAM;

  chunks += fread(&CPU, sizeof(CPU), 1, fp);
  chunks += fread(&ICPU, sizeof(ICPU), 1, fp);
  chunks += fread(&PPU, sizeof(PPU), 1, fp);
  chunks += fread(&DMA, sizeof(DMA), 1, fp);
  chunks += fread(Memory.VRAM, VRAM_SIZE, 1, fp);
  chunks += fread(Memory.RAM, RAM_SIZE, 1, fp);
  chunks += fread(Memory.SRAM, SRAM_SIZE, 1, fp);
  chunks += fread(Memory.FillRAM, FILLRAM_SIZE, 1, fp);
  chunks += fread(&APU, sizeof(APU), 1, fp);
  chunks += fread(&IAPU, sizeof(IAPU), 1, fp);

  uint8_t *IAPU_StaleRAM = IAPU.RAM; // Stale pointer from file
  IAPU.RAM = IAPU_RAM;               // Restore correct pointer
  chunks += fread(IAPU.RAM, 0x10000, 1, fp);
  chunks += fread(&SoundData, sizeof(SoundData), 1, fp);

  printf("Loaded chunks = %d / 12 (excluding header)\n", chunks);

  if (chunks != 12) {
    printf("Warning: Loaded state might be incomplete (%d/12 chunks read)\n",
           chunks);
  }

  // Fixing up registers and pointers:

  IAPU.PC = IAPU.PC - IAPU_StaleRAM + IAPU_RAM;
  IAPU.DirectPage = IAPU.DirectPage - IAPU_StaleRAM + IAPU_RAM;
  IAPU.WaitAddress1 = IAPU.WaitAddress1 - IAPU_StaleRAM + IAPU_RAM;
  IAPU.WaitAddress2 = IAPU.WaitAddress2 - IAPU_StaleRAM + IAPU_RAM;

  FixROMSpeed();
  IPPU.ColorsChanged = true;
  IPPU.OBJChanged = true;
  CPU.InDMA = false;
  CPU.PCAtOpcodeStart = NULL; // Invalidate stale pointers
  CPU.WaitAddress = NULL;
  S9xFixColourBrightness();
  S9xAPUUnpackStatus();
  S9xFixSoundAfterSnapshotLoad();
  ICPU.ShiftedPB = ICPU.Registers.PB << 16;
  ICPU.ShiftedDB = ICPU.Registers.DB << 16;
  S9xSetPCBase(ICPU.ShiftedPB + ICPU.Registers.PC);
  S9xUnpackStatus();
  S9xFixCycles();
  S9xReschedule();

  fclose(fp);
  return true;

fail:
  fclose(fp);
  return false;
}
