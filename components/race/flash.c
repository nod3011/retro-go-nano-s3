/*---------------------------------------------------------------------------
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version. See also the license.txt file for
 *	additional informations.
 *---------------------------------------------------------------------------
 */

/*
 * Flash chip emulation by Flavor
 *   with ideas from Koyote (who originally got ideas from Flavor :)
 * for emulation of NGPC carts
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"

#include <string.h>
// #include <streams/file_stream.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "flash.h"
#include "race-memory.h"
#include "retro_compat.h"
#include "types.h"

/* Manuf ID's
Supported
0x98		Toshiba
0xEC		Samsung
0xB0		Sharp

Other
0x89		Intel
0x01		AMD
0xBF		SST
*/
unsigned char manufID = 0x98; /* we're always Toshiba! */
unsigned char deviceID = 0x2F;
unsigned char cartSize = 32;
unsigned int bootBlockStartAddr = 0x1F0000;
unsigned char bootBlockStartNum = 31;

#include <rg_system.h>
#define SAVEGAME_DIR retro_save_directory

unsigned char currentWriteCycle = 1; /* can be 1 through 6 */
unsigned char currentCommand = NO_COMMAND;

#define FLASH_VALID_ID 0x0053

struct NGFheaderStruct {
  unsigned short version;   /* always 0x53? */
  unsigned short numBlocks; /* how many blocks are in the file */
  unsigned int fileLen;     /* length of the file */
};

struct blockStruct {
  unsigned int NGPCaddr; /* where this block starts (in NGPC memory map) */
  unsigned int len;      /* length of following data */
};

#define MAX_BLOCKS 35 /* a 16m chip has 35 blocks (SA0-SA34) */
unsigned char blocksDirty[2][MAX_BLOCKS]; /* max of 2 chips */
unsigned char needToWriteFile = 0;
char ngfFilename[256] = {0};

#define FLASH_WRITE 0
#define FLASH_ERASE 1

void setupFlashParams(void) {
  switch (cartSize) {
  default:
  case 32:
    deviceID = 0x2F; /* the upper chip will always be 16bit */
    bootBlockStartAddr = 0x1F0000;
    bootBlockStartNum = 31;
    break;
  case 16:
    deviceID = 0x2F;
    bootBlockStartAddr = 0x1F0000;
    bootBlockStartNum = 31;
    break;
  case 8:
    deviceID = 0x2C;
    bootBlockStartAddr = 0xF0000;
    bootBlockStartNum = 15;
    break;
  case 4:
    deviceID = 0xAB;
    bootBlockStartAddr = 0x70000;
    bootBlockStartNum = 7;
    break;
  case 0:
    manufID = 0x00;
    deviceID = 0x00;
    bootBlockStartAddr = 0x00000;
    bootBlockStartNum = 0;
    break;
  }
}

unsigned char blockNumFromAddr(unsigned int addr) {
  addr &= 0x1FFFFF /* & cartAddrMask*/;

  if (addr >= bootBlockStartAddr) {
    unsigned int bootAddr = addr - bootBlockStartAddr;
    /* boot block is 32k, 8k, 8k, 16k (0x8000,0x2000,0x2000,0x4000) */
    if (bootAddr < 0x8000)
      return (bootBlockStartAddr / 0x10000);
    else if (bootAddr < 0xA000)
      return (bootBlockStartAddr / 0x10000) + 1;
    else if (bootAddr < 0xC000)
      return (bootBlockStartAddr / 0x10000) + 2;
    else if (bootAddr < 0x10000)
      return (bootBlockStartAddr / 0x10000) + 3;
  }

  return addr / 0x10000;
}

unsigned int blockNumToAddr(unsigned char chip, unsigned char blockNum) {
  unsigned int addr;

  if (blockNum >= bootBlockStartNum) {
    unsigned char bootBlock;

    addr = bootBlockStartNum * 0x10000;
    bootBlock = blockNum - bootBlockStartNum;
    if (bootBlock >= 1)
      addr += 0x8000;
    if (bootBlock >= 2)
      addr += 0x2000;
    if (bootBlock >= 3)
      addr += 0x2000;
  } else
    addr = blockNum * 0x10000;

  if (chip)
    addr += 0x200000;

  return addr;
}

unsigned int blockSize(unsigned char blockNum) {
  if (blockNum >= bootBlockStartNum) {
    unsigned char bootBlock = blockNum - bootBlockStartNum;
    if (bootBlock == 3)
      return 0x4000;
    if (bootBlock == 2)
      return 0x2000;
    if (bootBlock == 1)
      return 0x2000;
    if (bootBlock == 0)
      return 0x8000;
  }

  return 0x10000;
}

void setupNGFfilename(void) {
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, rg_system_get_app()->romPath);
  if (path) {
    strncpy(ngfFilename, path, sizeof(ngfFilename) - 1);
    free(path);
  }
}

/* write all the dirty blocks out to a file */
void writeSaveGameFile(void) {
  /* find the dirty blocks and write them to the .NGF file */
  int totalBlocks = bootBlockStartNum + 4;
  int i;
  struct NGFheaderStruct NGFheader;
  struct blockStruct block;

  setupNGFfilename();

  NGFheader.version = 0x53;
  NGFheader.numBlocks = 0;
  NGFheader.fileLen = sizeof(struct NGFheaderStruct);
  /* add them all up, first */
  for (i = 0; i < totalBlocks; i++) {
    if (blocksDirty[0][i]) {
      NGFheader.numBlocks++;
      NGFheader.fileLen += blockSize(i);
    }
  }

  if (cartSize == 32) /* do the second chip, also */
  {
    for (i = 0; i < totalBlocks; i++) {
      if (blocksDirty[1][i]) {
        NGFheader.numBlocks++;
        NGFheader.fileLen += blockSize(i);
      }
    }
  }

  NGFheader.fileLen += NGFheader.numBlocks * sizeof(struct blockStruct);

  unsigned char *buffer = malloc(NGFheader.fileLen);
  if (!buffer)
    return;
  size_t offset = 0;

  memcpy(buffer + offset, &NGFheader, sizeof(struct NGFheaderStruct));
  offset += sizeof(struct NGFheaderStruct);

  for (i = 0; i < totalBlocks; i++) {
    if (blocksDirty[0][i]) {
      block.NGPCaddr = blockNumToAddr(0, i) + 0x200000;
      block.len = blockSize(i);

      memcpy(buffer + offset, &block, sizeof(struct blockStruct));
      offset += sizeof(struct blockStruct);

      unsigned int blkAddr = blockNumToAddr(0, i);
      unsigned int blkSize = blockSize(i);
      if (blkAddr + blkSize <= m_emuInfo.romSize) {
        memcpy(buffer + offset, &mainrom[blkAddr], blkSize);
      } else {
        memset(buffer + offset, 0xFF, blkSize);
        if (blkAddr < m_emuInfo.romSize) {
          memcpy(buffer + offset, &mainrom[blkAddr],
                 m_emuInfo.romSize - blkAddr);
        }
      }
      offset += blkSize;
    }
  }

  if (cartSize == 32) /* do the second chip, also */
  {
    for (i = 0; i < totalBlocks; i++) {
      if (blocksDirty[1][i]) {
        block.NGPCaddr = blockNumToAddr(1, i) + 0x600000;
        block.len = blockSize(i);

        memcpy(buffer + offset, &block, sizeof(struct blockStruct));
        offset += sizeof(struct blockStruct);

        unsigned int blkAddr = blockNumToAddr(1, i);
        unsigned int blkSize = blockSize(i);
        if (blkAddr + blkSize <= m_emuInfo.romSize) {
          memcpy(buffer + offset, &mainrom[blkAddr], blkSize);
        } else {
          memset(buffer + offset, 0xFF, blkSize);
          if (blkAddr < m_emuInfo.romSize) {
            memcpy(buffer + offset, &mainrom[blkAddr],
                   m_emuInfo.romSize - blkAddr);
          }
        }
        offset += blkSize;
      }
    }
  }

  rg_storage_write_file(ngfFilename, buffer, NGFheader.fileLen, 0);
  free(buffer);

  needToWriteFile = 0;
}

/* read the save-game file and overlay it onto mainrom */
void loadSaveGameFile(void) {
  setupNGFfilename();

  void *fileData = NULL;
  size_t fileSize = 0;
  if (!rg_storage_read_file(ngfFilename, &fileData, &fileSize, 0)) {
    return;
  }

  if (fileSize < sizeof(struct NGFheaderStruct)) {
    free(fileData);
    return;
  }

  struct NGFheaderStruct *NGFheader = (struct NGFheaderStruct *)fileData;

  if (NGFheader->version != 0x53 || NGFheader->numBlocks > MAX_BLOCKS ||
      fileSize < NGFheader->fileLen) {
    free(fileData);
    return;
  }

  unsigned char *blocks =
      (unsigned char *)fileData + sizeof(struct NGFheaderStruct);

  /* loop through the blocks and insert them into mainrom */
  for (int i = 0; i < NGFheader->numBlocks; i++) {
    struct blockStruct *blockHeader = (struct blockStruct *)blocks;
    blocks += sizeof(struct blockStruct);

    if (!((blockHeader->NGPCaddr >= 0x200000 &&
           blockHeader->NGPCaddr < 0x400000) ||
          (blockHeader->NGPCaddr >= 0x800000 &&
           blockHeader->NGPCaddr < 0xA00000))) {
      free(fileData);
      return;
    }
    if (blockHeader->NGPCaddr >= 0x800000) {
      blockHeader->NGPCaddr -= 0x600000;
      blocksDirty[1][blockNumFromAddr(blockHeader->NGPCaddr - 0x200000)] = 1;
    } else if (blockHeader->NGPCaddr >= 0x200000) {
      blockHeader->NGPCaddr -= 0x200000;
      blocksDirty[0][blockNumFromAddr(blockHeader->NGPCaddr)] = 1;
    }

    if (blockHeader->NGPCaddr + blockHeader->len <= m_emuInfo.romSize) {
      memcpy((void *)&mainrom[blockHeader->NGPCaddr], blocks, blockHeader->len);
    }

    blocks += blockHeader->len;
  }

  free(fileData);
}

void flashWriteByte(unsigned int addr, unsigned char data,
                    unsigned char operation) {
  if (blockNumFromAddr(addr) == 0) /* hack because DWARP writes to bank 0 */
    return;

  /* set a dirty flag for the block that we are writing to */
  if (addr < 0x200000) {
    blocksDirty[0][blockNumFromAddr(addr)] = 1;
    needToWriteFile = 1;
  } else if (addr < 0x400000) {
    blocksDirty[1][blockNumFromAddr(addr)] = 1;
    needToWriteFile = 1;
  } else
    return; /* panic */

  /* changed to &= because it's actually how flash works
   * flash memory can be erased (changed to 0xFF)
   * and when written, 1s can become 0s, but you can't turn 0s into 1s (except
   * by erasing)
   */
  if (addr < m_emuInfo.romSize) {
    unsigned char *dst = (unsigned char *)mainrom;
    if (operation == FLASH_ERASE)
      dst[addr] = 0xFF; /* we're just erasing, so set to 0xFF */
    else
      dst[addr] &= data; /* actually writing data */
  }
}

unsigned char flashReadInfo(unsigned int addr) {
  currentWriteCycle = 1;
  currentCommand = COMMAND_INFO_READ;

  switch (addr & 0x03) {
  case 0:
    return manufID;
  case 1:
    return deviceID;
  case 2:
    return 0; /* block not protected */
  case 3:     /* thanks Koyote */
  default:
    return 0x80;
  }
}

void flashChipWrite(unsigned int addr, unsigned char data) {
  if (addr >= 0x800000 && cartSize != 32)
    return;

  switch (currentWriteCycle) {
  case 1:
    if ((addr & 0xFFFF) == 0x5555 && data == 0xAA)
      currentWriteCycle++;
    else if (data == 0xF0) {
      currentWriteCycle = 1; /* this is a reset command */
      needToWriteFile = 1;
    } else
      currentWriteCycle = 1;

    currentCommand = NO_COMMAND;
    break;
  case 2:
    if ((addr & 0xFFFF) == 0x2AAA && data == 0x55)
      currentWriteCycle++;
    else
      currentWriteCycle = 1;

    currentCommand = NO_COMMAND;
    break;
  case 3:
    if ((addr & 0xFFFF) == 0x5555 && data == 0x80)
      currentWriteCycle++; /* continue on */
    else if ((addr & 0xFFFF) == 0x5555 && data == 0xF0) {
      currentWriteCycle = 1;
      needToWriteFile = 1;
    } else if ((addr & 0xFFFF) == 0x5555 && data == 0x90) {
      currentWriteCycle++;
      currentCommand = COMMAND_INFO_READ;
      /* now, the next time we read from flash,
       * we should return a ID value
       * or a block protect value */
      break;
    } else if ((addr & 0xFFFF) == 0x5555 && data == 0xA0) {
      currentWriteCycle++;
      currentCommand = COMMAND_BYTE_PROGRAM;
      break;
    } else
      currentWriteCycle = 1;

    currentCommand = NO_COMMAND;
    break;

  case 4:
    /* time to write to flash memory */
    if (currentCommand == COMMAND_BYTE_PROGRAM) {
      unsigned char chip = 0;
      if (addr >= 0x200000 && addr < 0x400000) {
        addr -= 0x200000;
        chip = 0;
      } else if (addr >= 0x800000 && addr < 0xA00000) {
        addr -= 0x600000;
        chip = 1;
      }

      vectFlashWrite(chip, addr, &data, 1);
      currentWriteCycle = 1;
      currentCommand = NO_COMMAND;
      needToWriteFile = 1;
    } else if ((addr & 0xFFFF) == 0x5555 && data == 0xAA)
      currentWriteCycle++;
    else
      currentWriteCycle = 1;

    currentCommand = NO_COMMAND;
    break;
  case 5:
    if ((addr & 0xFFFF) == 0x2AAA && data == 0x55)
      currentWriteCycle++;
    else
      currentWriteCycle = 1;

    currentCommand = NO_COMMAND;
    break;
  case 6:
    /* chip erase */
    if ((addr & 0xFFFF) == 0x5555 && data == 0x10) {
      currentWriteCycle = 1;
      currentCommand = COMMAND_CHIP_ERASE;

      /* erase the entire chip
       * memset it to all 0xFF
       * I think we won't implement this
       */

      break;
    }
    /* block erase */
    if (data == 0x30 || data == 0x50) {
      unsigned char chip = 0;
      currentWriteCycle = 1;
      currentCommand = COMMAND_BLOCK_ERASE;

      /* erase the entire block that contains addr
       * memset it to all 0xFF */

      if (addr >= 0x800000)
        chip = 1;

      vectFlashErase(chip, blockNumFromAddr(addr));
      needToWriteFile = 1;
      break;
    } else
      currentWriteCycle = 1;

    currentCommand = NO_COMMAND;
    break;

  default:
    currentWriteCycle = 1;
    currentCommand = NO_COMMAND;
    break;
  }
}

/* this should be called when a ROM is unloaded */
void flashShutdown(void) { writeSaveGameFile(); }

/* this should be called when a ROM is loaded */
void flashStartup(void) {
  memset(blocksDirty[0], 0, MAX_BLOCKS * sizeof(blocksDirty[0][0]));
  memset(blocksDirty[1], 0, MAX_BLOCKS * sizeof(blocksDirty[0][0]));
  needToWriteFile = 0;

  loadSaveGameFile();
}

void vectFlashWrite(unsigned char chip, unsigned int to,
                    unsigned char *fromAddr, unsigned int numBytes) {

  if (chip)
    to += 0x200000;

  while (numBytes--) {
    flashWriteByte(to, *fromAddr, FLASH_WRITE);
    fromAddr++;
    to++;
  }
}

void vectFlashErase(unsigned char chip, unsigned char blockNum) {
  /* this needs to be modified to take into account boot block areas (less than
   * 64k) */
  unsigned int blockAddr = blockNumToAddr(chip, blockNum);
  unsigned int numBytes = blockSize(blockNum);

  while (numBytes--) {
    flashWriteByte(blockAddr, 0xFF, FLASH_ERASE);
    blockAddr++;
  }
}

void vectFlashChipErase(unsigned char chip) {}

void setFlashSize(unsigned int romSize) {
  /* add individual hacks here. */

  /*delta warp */
  if (strncmp((const char *)&mainrom[0x24], "DELTA WARP ", 11) == 0)
    cartSize = 8; /* 1 8mbit chip */
  else if (romSize > 0x200000)
    cartSize = 32; /* 2 16mbit chips */
  else if (romSize > 0x100000)
    cartSize = 16;             /* 1 16mbit chip */
  else if (romSize > 0x080000) /* 1 8mbit chip */
    cartSize = 8;
  else if (romSize > 0x040000) /* 1 4mbit chip */
    cartSize = 4;
  else if (romSize == 0) /* no cart, just emu BIOS */
    cartSize = 0;
  else {
    /* we don't know.  It's probably a homebrew or something cut down
     * so just pretend we're a Bung! cart
     * 2 16mbit chips */
    cartSize = 32;
  }

  setupFlashParams();

  flashStartup();
}
