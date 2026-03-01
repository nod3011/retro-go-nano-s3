/*---------------------------------------------------------------------------
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version. See also the license.txt file for
 *  additional informations.
 *---------------------------------------------------------------------------

 * state.cpp: state saving
 *
 *  01/20/2009 Cleaned up interface, added loading from memory
 *             Moved signature-related stuff out of race_state (A.K.)
 *  09/11/2008 Initial version (Akop Karapetyan)
 */

#include "cz80.h"
#ifdef DRZ80
#include "DrZ80_support.h"
#endif
#include "neopopsound.h"

#include "race-memory.h"
#include "retro_compat.h"
#include "state.h"
#include "tlcs900h.h"
#include <string.h>

extern unsigned char *s_cpuram_256;
extern int is_mono_game;
extern unsigned char interruptPendingLevel;
extern unsigned char pendingInterrupts[7][4];
extern int dacLBufferRead, dacLBufferWrite, dacLBufferCount;
extern u16 *dacBufferL;
extern int fixsoundmahjong;
extern int finscan, contador;
extern int DMAstate;
extern int tlcsClockMulti;
extern int ngOverflow;
extern unsigned char *my_pc;

#ifdef PC
#undef PC
#endif

#define CURRENT_SAVE_STATE_VERSION 0x12

struct race_state_header {
  u8 state_version;       /* State version */
  u8 rom_signature[0x40]; /* Rom signature, for verification */
};

struct race_state_0x12 {
  /* Memory */
  u8 ram[0x14000];
  u8 cpuram[0x1000];

  /* TLCS-900h Registers */
  u32 pc, sr;
  u8 f_dash;
  u32 gpr[23];

  /* Z80 Registers */
#ifdef CZ80
  cz80_struc RACE_cz80_struc;
  u32 PC_offset;
  s32 Z80_ICount;
#elif DRZ80
  struct Z80_Regs Z80;
#endif
  /* Sound */
  int sndCycles;
  SoundChip toneChip;
  SoundChip noiseChip;

  /* DAC State */
  int dacLBufferRead;
  int dacLBufferWrite;
  int dacLBufferCount;
  u16 dacBufferL[4096];
  int fixsoundmahjong;

  /* Timers */
  int timer0, timer1, timer2, timer3;

  /* DMA */
  u8 ldcRegs[64];

  /* Interrupt State */
  u8 interruptPendingLevel;
  u8 pendingInterrupts[7][4];

  /* System State */
  u8 machine_type;
  u8 is_mono;
  int finscan;
  int contador;
  int DMAstate;
  int tlcsClockMulti;
  int ngOverflow;
};

struct race_state_0x10 /* Older state format */
{
  /* Save state version */
  u8 state_version; /* = 0x10 */

  /* Rom signature */
  u8 rom_signature[0x40];

  /* Memory */
  u8 ram[0xc000];
  u8 cpuram[0x08a0]; /* 0xC000]; 0x38000  */

  /* TLCS-900h Registers */
  u32 pc, sr;
  u8 f_dash;
  u32 gpr[23];

  /* Z80 Registers */
#ifdef CZ80
  cz80_struc RACE_cz80_struc;
  u32 PC_offset;
  s32 Z80_ICount;
#elif DRZ80
  struct Z80_Regs Z80;
#endif

  /* Sound Chips */
  int sndCycles;
  SoundChip toneChip;
  SoundChip noiseChip;

  /* Timers */
  int timer0, timer1, timer2, timer3;

  /* DMA */
  u8 ldcRegs[64];
};

typedef struct race_state_0x12 race_state_t;

static int state_store(race_state_t *rs) {
  int i = 0;
#ifdef CZ80
  extern cz80_struc *RACE_cz80_struc;
  extern s32 Z80_ICount;
  int size_of_z80;
#elif DRZ80
  extern struct Z80_Regs Z80;
#endif
  extern int sndCycles;

  /* TLCS-900h Registers */
  rs->pc = gen_regsPC;
  rs->sr = gen_regsSR;
  rs->f_dash = F2;

  rs->gpr[i++] = gen_regsXWA0;
  rs->gpr[i++] = gen_regsXBC0;
  rs->gpr[i++] = gen_regsXDE0;
  rs->gpr[i++] = gen_regsXHL0;

  rs->gpr[i++] = gen_regsXWA1;
  rs->gpr[i++] = gen_regsXBC1;
  rs->gpr[i++] = gen_regsXDE1;
  rs->gpr[i++] = gen_regsXHL1;

  rs->gpr[i++] = gen_regsXWA2;
  rs->gpr[i++] = gen_regsXBC2;
  rs->gpr[i++] = gen_regsXDE2;
  rs->gpr[i++] = gen_regsXHL2;

  rs->gpr[i++] = gen_regsXWA3;
  rs->gpr[i++] = gen_regsXBC3;
  rs->gpr[i++] = gen_regsXDE3;
  rs->gpr[i++] = gen_regsXHL3;

  rs->gpr[i++] = gen_regsXIX;
  rs->gpr[i++] = gen_regsXIY;
  rs->gpr[i++] = gen_regsXIZ;
  rs->gpr[i++] = gen_regsXSP;

  rs->gpr[i++] = gen_regsSP;
  rs->gpr[i++] = gen_regsXSSP;
  rs->gpr[i++] = gen_regsXNSP;

  /* Z80 Registers */
#ifdef CZ80
  size_of_z80 = (uintptr_t)(&(RACE_cz80_struc->CycleSup)) -
                (uintptr_t)(&(RACE_cz80_struc->BC));
  memcpy(&rs->RACE_cz80_struc, RACE_cz80_struc, size_of_z80);
  rs->Z80_ICount = Z80_ICount;
  rs->PC_offset = Cz80_Get_PC(RACE_cz80_struc);
#elif DRZ80
  memcpy(&rs->Z80, &Z80, sizeof(Z80));
#endif

  /* Sound */
  rs->sndCycles = sndCycles;
  memcpy(&rs->toneChip, &toneChip, sizeof(SoundChip));
  memcpy(&rs->noiseChip, &noiseChip, sizeof(SoundChip));

  /* DAC State */
  rs->dacLBufferRead = dacLBufferRead;
  rs->dacLBufferWrite = dacLBufferWrite;
  rs->dacLBufferCount = dacLBufferCount;
  if (dacBufferL)
    memcpy(rs->dacBufferL, dacBufferL, 4096 * sizeof(u16));
  rs->fixsoundmahjong = fixsoundmahjong;

  /* Timers */
  rs->timer0 = timer0;
  rs->timer1 = timer1;
  rs->timer2 = timer2;
  rs->timer3 = timer3;

  /* DMA */
  memcpy(&rs->ldcRegs, &ldcRegs, sizeof(ldcRegs));

  /* Interrupt State */
  rs->interruptPendingLevel = interruptPendingLevel;
  memcpy(rs->pendingInterrupts, pendingInterrupts, sizeof(pendingInterrupts));

  /* Memory */
  memcpy(rs->ram, mainram, 0x14000);
  if (s_cpuram_256)
    memcpy(rs->cpuram, s_cpuram_256, 0x1000);

  /* System State */
  rs->machine_type = (u8)m_emuInfo.machine;
  rs->is_mono = (u8)is_mono_game;
  rs->finscan = finscan;
  rs->contador = contador;
  rs->DMAstate = DMAstate;
  rs->tlcsClockMulti = tlcsClockMulti;
  rs->ngOverflow = ngOverflow;

  return 1;
}

static int state_restore(race_state_t *rs) {
  int i = 0;
#ifdef CZ80
  extern cz80_struc *RACE_cz80_struc;
  extern s32 Z80_ICount;
  int size_of_z80;
#elif DRZ80
  extern struct Z80_Regs Z80;
#endif
  extern int sndCycles;
  tlcs_reinit();

  /* TLCS-900h Registers */
  gen_regsPC = rs->pc;
  gen_regsSR = rs->sr;
  F2 = rs->f_dash;

  gen_regsXWA0 = rs->gpr[i++];
  gen_regsXBC0 = rs->gpr[i++];
  gen_regsXDE0 = rs->gpr[i++];
  gen_regsXHL0 = rs->gpr[i++];

  gen_regsXWA1 = rs->gpr[i++];
  gen_regsXBC1 = rs->gpr[i++];
  gen_regsXDE1 = rs->gpr[i++];
  gen_regsXHL1 = rs->gpr[i++];

  gen_regsXWA2 = rs->gpr[i++];
  gen_regsXBC2 = rs->gpr[i++];
  gen_regsXDE2 = rs->gpr[i++];
  gen_regsXHL2 = rs->gpr[i++];

  gen_regsXWA3 = rs->gpr[i++];
  gen_regsXBC3 = rs->gpr[i++];
  gen_regsXDE3 = rs->gpr[i++];
  gen_regsXHL3 = rs->gpr[i++];

  gen_regsXIX = rs->gpr[i++];
  gen_regsXIY = rs->gpr[i++];
  gen_regsXIZ = rs->gpr[i++];
  gen_regsXSP = rs->gpr[i++];

  gen_regsSP = rs->gpr[i++];
  gen_regsXSSP = rs->gpr[i++];
  gen_regsXNSP = rs->gpr[i++];

  /* Z80 Registers */
#ifdef CZ80
  size_of_z80 = (uintptr_t)(&(RACE_cz80_struc->CycleSup)) -
                (uintptr_t)(&(RACE_cz80_struc->BC));

  memcpy(RACE_cz80_struc, &rs->RACE_cz80_struc, size_of_z80);
  Z80_ICount = rs->Z80_ICount;
  Cz80_Set_PC(RACE_cz80_struc, rs->PC_offset);
#elif DRZ80
  memcpy(&Z80, &rs->Z80, sizeof(Z80));
#endif

  /* Sound */
  sndCycles = rs->sndCycles;
  memcpy(&toneChip, &rs->toneChip, sizeof(SoundChip));
  memcpy(&noiseChip, &rs->noiseChip, sizeof(SoundChip));

  /* DAC State */
  dacLBufferRead = rs->dacLBufferRead;
  dacLBufferWrite = rs->dacLBufferWrite;
  dacLBufferCount = rs->dacLBufferCount;
  if (dacBufferL)
    memcpy(dacBufferL, rs->dacBufferL, 4096 * sizeof(u16));
  fixsoundmahjong = rs->fixsoundmahjong;

  /* Timers */
  timer0 = rs->timer0;
  timer1 = rs->timer1;
  timer2 = rs->timer2;
  timer3 = rs->timer3;

  /* DMA */
  memcpy(&ldcRegs, &rs->ldcRegs, sizeof(ldcRegs));

  /* Interrupt State */
  interruptPendingLevel = rs->interruptPendingLevel;
  memcpy(pendingInterrupts, rs->pendingInterrupts, sizeof(pendingInterrupts));

  /* Memory */
  memcpy(mainram, rs->ram, 0x14000);
  if (s_cpuram_256)
    memcpy(s_cpuram_256, rs->cpuram, 0x1000);

  /* System State */
  m_emuInfo.machine = rs->machine_type;
  is_mono_game = (int)rs->is_mono;
  finscan = rs->finscan;
  contador = rs->contador;
  DMAstate = rs->DMAstate;
  tlcsClockMulti = rs->tlcsClockMulti;
  ngOverflow = rs->ngOverflow;

  /* Update my_pc manually after restoring all state and memory */
  my_pc = (unsigned char *)get_address(gen_regsPC);

  return 1;
}

int state_store_mem(void *state) { return state_store((race_state_t *)state); }

int state_restore_mem(void *state) {
  return state_restore((race_state_t *)state);
}

int state_get_size(void) { return sizeof(race_state_t); }
