#if defined(MOKA_GAMEBOY)

// The firmware is built for size (-Os); the emulator's CPU loop runs much
// faster optimised for speed.
#pragma GCC optimize("O2")

// The emulator itself: Peanut-GB (MIT), compiled once here as C.
#define ENABLE_SOUND 0
#define ENABLE_LCD   1
#include "peanut_gb.h"
#include "gameboyCore.h"

static struct gb_s gb;
static const uint8_t *rom = NULL;
static size_t romSize = 0;
static uint8_t *cartRam = NULL;
static size_t cartRamSize = 0;
static volatile bool ramDirty = false;
static volatile bool failed = false;
static bool drawFrame = true;

static uint8_t romRead(struct gb_s *g, const uint_fast32_t addr) {
  (void)g;
  return addr < romSize ? rom[addr] : 0xFF;
}

static uint8_t cartRamRead(struct gb_s *g, const uint_fast32_t addr) {
  (void)g;
  return addr < cartRamSize ? cartRam[addr] : 0xFF;
}

static void cartRamWrite(struct gb_s *g, const uint_fast32_t addr, const uint8_t value) {
  (void)g;
  if (addr < cartRamSize && cartRam[addr] != value) {
    cartRam[addr] = value;
    ramDirty = true;
  }
}

static void onError(struct gb_s *g, const enum gb_error_e error, const uint16_t addr) {
  (void)g; (void)error; (void)addr;
  failed = true;
}

static void drawLine(struct gb_s *g, const uint8_t *pixels, const uint_fast8_t line) {
  (void)g;
  if (drawFrame) gbcoreDrawLine(pixels, (uint8_t)line);
}

int gbcoreInit(const uint8_t *romData, size_t size) {
  rom = romData;
  romSize = size;
  failed = false;
  enum gb_init_error_e result = gb_init(&gb, romRead, cartRamRead, cartRamWrite, onError, NULL);
  if (result != GB_INIT_NO_ERROR) return (int)result;
  gb_init_lcd(&gb, drawLine);
  return 0;
}

long gbcoreSaveSize(void) {
  size_t size = 0;
  if (gb_get_save_size_s(&gb, &size) != 0) return -1;
  return (long)size;
}

void gbcoreSetCartRam(uint8_t *ram, size_t size) {
  cartRam = ram;
  cartRamSize = size;
}

bool gbcoreTakeRamDirty(void) {
  bool dirty = ramDirty;
  ramDirty = false;
  return dirty;
}

void gbcoreRomName(char *title17) {
  gb_get_rom_name(&gb, title17);
}

void gbcoreSetButtons(uint8_t pressedMask) {
  // Peanut-GB's joypad is active low: a cleared bit is a pressed button.
  gb.direct.joypad = (uint8_t)~pressedMask;
}

void gbcoreRunFrame(bool draw) {
  drawFrame = draw;
  gb_run_frame(&gb);
}

bool gbcoreFailed(void) {
  return failed;
}

#endif // MOKA_GAMEBOY
