#pragma once

// Thin C wrapper around Peanut-GB (lib/PeanutGB/peanut_gb.h), so the C++
// front-end (gameboy.cpp) never includes the emulator header itself.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Buttons for gbcoreSetButtons(), as a mask of pressed buttons.
#define GB_BUTTON_A       0x01
#define GB_BUTTON_B       0x02
#define GB_BUTTON_SELECT  0x04
#define GB_BUTTON_START   0x08
#define GB_BUTTON_RIGHT   0x10
#define GB_BUTTON_LEFT    0x20
#define GB_BUTTON_UP      0x40
#define GB_BUTTON_DOWN    0x80

#define GB_SCREEN_WIDTH   160
#define GB_SCREEN_HEIGHT  144

// Starts the emulator on a ROM held in memory (kept, not copied). Returns 0,
// or Peanut-GB's error (1 unsupported cartridge, 2 bad header checksum).
int gbcoreInit(const uint8_t *rom, size_t romSize);
// Battery-backed cartridge RAM the game needs (0: none, -1: unsupported).
long gbcoreSaveSize(void);
// The cartridge RAM buffer (gbcoreSaveSize() bytes, filled from the .sav).
void gbcoreSetCartRam(uint8_t *ram, size_t size);
// True when the game wrote to its cartridge RAM since the last call.
bool gbcoreTakeRamDirty(void);
// Game title from the ROM header (up to 16 characters).
void gbcoreRomName(char *title17);
void gbcoreSetButtons(uint8_t pressedMask);
// Runs one Game Boy frame (1/59.7 s of emulated time). When draw is false the
// lines are not handed to gbcoreDrawLine() (frames that are not shown).
void gbcoreRunFrame(bool draw);
// Set when the emulated CPU hit something it cannot run (invalid opcode...).
bool gbcoreFailed(void);

// Implemented by the front-end: one screen line, GB_SCREEN_WIDTH pixels;
// bits 0-1 are the shade (0 lightest), bits 4-5 the palette.
void gbcoreDrawLine(const uint8_t *pixels, uint8_t line);

#ifdef __cplusplus
}
#endif
