#if defined(MOKA_GAMEBOY)

// Game Boy easter egg. Swipe down on the main screen: the moka switches OFF,
// LVGL stops drawing, and this task owns the screen. It lists the .gb ROMs in
// the SD card's root folder, runs the chosen one with Peanut-GB (see
// gameboyCore.c) and draws the controls on the screen (the board has no
// buttons). Swipe up on the game picture to leave: the battery save is
// written and the board restarts into MokaBot.
//
// Screen (480x320): the game, scaled x2 (320x288), in the middle; d-pad and
// SELECT on the left, A, B and START on the right. The d-pad is large and
// drawn over the left edge of the game picture. Two fingers at once work (a
// direction and A, for example). Colours follow the moka's button colour.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include "gameboy.hpp"
#include "gameboyCore.h"
#include "devices/jc3248w535.hpp"
#include "logic/moka.hpp"
#include "taskCores.hpp"
#include "ui/ui.h"

extern SemaphoreHandle_t lvglMutex;   // main.cpp

// SD card on its own SPI bus (the display uses SPI2 for QSPI).
#define SD_CS    10
#define SD_MOSI  11
#define SD_SCK   12
#define SD_MISO  13
#define SD_HZ    20000000

#define MAX_ROMS        64
#define MAX_ROM_BYTES   (8UL * 1024 * 1024)   // largest Game Boy cartridge

// Layout
#define GAME_X        80
#define GAME_Y        16
#define GAME_SCALE    2
#define GAME_W        (GB_SCREEN_WIDTH * GAME_SCALE)
#define GAME_H        (GB_SCREEN_HEIGHT * GAME_SCALE)
#define DPAD_X        78      // centre; the right arm covers the game's left edge
#define DPAD_Y        146
#define DPAD_ARM_W    46      // width of each arm
#define DPAD_ARM_L    74      // centre to the tip of an arm
#define DPAD_REACH    112     // touches within this of the d-pad centre
#define DPAD_DEAD     14      // ... and further than this from it
// A and B as on the original: B lower left, A upper right, both partly over
// the right edge of the game picture.
#define BUTTON_A_X    440
#define BUTTON_A_Y    118
#define BUTTON_B_X    368
#define BUTTON_B_Y    164
#define BUTTON_R      34
#define BUTTON_REACH  46
#define SMALL_Y       262     // SELECT (left) and START (right)
#define SMALL_H       34
#define SELECT_X      6
#define START_X       406
#define SMALL_W       68
#define EXIT_SWIPE    80      // px upwards on the game picture to leave

// Timing: the Game Boy runs at 59.73 frames/s. Sending a full frame to the
// panel takes ~35 ms, so it is done by a task on the other core: the game
// keeps its speed and the screen shows the newest frame whenever it is free
// (about 25 frames/s).
#define FRAME_US      16743
#define SAVE_EVERY_MS 30000   // battery save written at most this often

#define COLOR_BG      0x0000
#define COLOR_LABEL   0xFFFF
#define COLOR_TEXT    0xFFFF
#define COLOR_DIM     0x8410

static Arduino_GFX *gfx = nullptr;
static void waitDisplayIdle();
static SPIClass sdSpi(HSPI);
static volatile bool running = false;

static uint16_t rgb565(uint32_t rgb) {
  return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
}

// The moka's button colour (see setColours): the controls, and the game's
// four shades, lightest first.
static uint16_t COLOR_BUTTON = 0x4208;
static uint16_t COLOR_PRESSED = 0x8410;
static uint32_t buttonRgb = 0x404040, pressedRgb = 0x808080;
static uint32_t paletteRgb[4];

// a * (1 - f) + b * f, per channel (24-bit RGB).
static uint32_t mixRgb(uint32_t a, uint32_t b, float f) {
  uint32_t out = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    float ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
    out |= ((uint32_t)(ca + (cb - ca) * f + 0.5f) & 0xFF) << shift;
  }
  return out;
}

static void setColours() {
  uint32_t theme = ui_get_button_color() & 0xFFFFFF;
  buttonRgb = theme;
  pressedRgb = mixRgb(theme, 0xFFFFFF, 0.45f);
  COLOR_BUTTON = rgb565(buttonRgb);
  COLOR_PRESSED = rgb565(pressedRgb);
  paletteRgb[0] = mixRgb(theme, 0xFFFFFF, 0.80f);   // Game Boy "white"
  paletteRgb[1] = mixRgb(theme, 0xFFFFFF, 0.15f);
  paletteRgb[2] = mixRgb(theme, 0x000000, 0.50f);
  paletteRgb[3] = mixRgb(theme, 0x000000, 0.85f);   // Game Boy "black"
}

// The emulator draws into frameWork (internal RAM, cheap); a finished frame
// is copied to frameShown for the display task (see displayTask), which
// scales it into the screen and sends it while the next frames run.
static uint8_t frameWork[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];
static uint8_t frameShown[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];

extern "C" void gbcoreDrawLine(const uint8_t *pixels, uint8_t line) {
  memcpy(frameWork + line * GB_SCREEN_WIDTH, pixels, GB_SCREEN_WIDTH);
}

// ---- Touch -----------------------------------------------------------------

struct Touches {
  int count = 0;        // fingers down
  int16_t x[2], y[2];
};

// Last state; frames without new data keep it.
static Touches touches;

static void readTouches() {
  Touches fresh;
  int count = displayReadTouchPoints(fresh.x, fresh.y, 2);
  if (count < 0) return;
  fresh.count = count;
  touches = fresh;
}

static bool nearDpad(int16_t x, int16_t y) {
  int dx = x - DPAD_X, dy = y - DPAD_Y;
  return dx * dx + dy * dy <= DPAD_REACH * DPAD_REACH;
}

static bool nearAB(int16_t x, int16_t y) {
  int ax = x - BUTTON_A_X, ay = y - BUTTON_A_Y, bx = x - BUTTON_B_X, by = y - BUTTON_B_Y;
  return ax * ax + ay * ay <= BUTTON_REACH * BUTTON_REACH || bx * bx + by * by <= BUTTON_REACH * BUTTON_REACH;
}

// The game picture, outside the controls drawn over it (for the exit swipe).
static bool inGame(int16_t x, int16_t y) {
  return x >= GAME_X && x < GAME_X + GAME_W && y >= GAME_Y && y < GAME_Y + GAME_H &&
         !nearDpad(x, y) && !nearAB(x, y);
}

// Buttons pressed by one finger.
static uint8_t buttonsAt(int16_t x, int16_t y) {
  uint8_t pressed = 0;
  int dx = x - DPAD_X, dy = y - DPAD_Y;
  if (dx * dx + dy * dy <= DPAD_REACH * DPAD_REACH && dx * dx + dy * dy >= DPAD_DEAD * DPAD_DEAD) {
    // Diagonals count both directions.
    if (abs(dx) * 5 >= abs(dy) * 2) pressed |= dx > 0 ? GB_BUTTON_RIGHT : GB_BUTTON_LEFT;
    if (abs(dy) * 5 >= abs(dx) * 2) pressed |= dy > 0 ? GB_BUTTON_DOWN : GB_BUTTON_UP;
  }
  int ax = x - BUTTON_A_X, ay = y - BUTTON_A_Y;
  int bx = x - BUTTON_B_X, by = y - BUTTON_B_Y;
  bool nearA = ax * ax + ay * ay <= BUTTON_REACH * BUTTON_REACH;
  bool nearB = bx * bx + by * by <= BUTTON_REACH * BUTTON_REACH;
  if (nearA && (!nearB || ax * ax + ay * ay <= bx * bx + by * by)) pressed |= GB_BUTTON_A;
  else if (nearB) pressed |= GB_BUTTON_B;
  if (y >= SMALL_Y && y < SMALL_Y + SMALL_H) {
    if (x >= SELECT_X && x < SELECT_X + SMALL_W) pressed |= GB_BUTTON_SELECT;
    if (x >= START_X && x < START_X + SMALL_W) pressed |= GB_BUTTON_START;
  }
  return pressed;
}

// ---- Drawing ---------------------------------------------------------------

static void centredText(const char *text, int16_t cx, int16_t cy, uint8_t size, uint16_t color) {
  int16_t bx, by;
  uint16_t w, h;
  gfx->setTextSize(size);
  gfx->getTextBounds(text, 0, 0, &bx, &by, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(text);
}

// The controls are drawn once, at the start, into an overlay mask: one id per
// pixel of the band the game is in (the full screen width, GAME_H rows). The
// display task gives each id a colour for the current frame (colourTable),
// blended with the game pixel under it, so the game shows through the parts
// of the controls drawn over it; around the game they are solid.
#define ID_UP       1
#define ID_DOWN     2
#define ID_LEFT     3
#define ID_RIGHT    4
#define ID_CENTRE   5
#define ID_A        6
#define ID_B        7
#define ID_SELECT   8
#define ID_START    9
#define ID_OUTLINE  14
#define ID_LABEL    15
#define OVER_GAME_ALPHA   0.45f   // controls over the game: 45 % control, 55 % game
#define LABEL_ALPHA       0.85f

static uint8_t *overlay = nullptr;        // screenWidth x GAME_H ids, column by column
static uint8_t columnHasOverlay[screenWidth];
static uint16_t colourTable[16][5];

// Draws text centred on (cx, cy) with Arduino_GFX's built-in font.
static void centredTextOn(Arduino_GFX *on, const char *text, int16_t cx, int16_t cy, uint8_t size, uint16_t colour) {
  int16_t bx, by;
  uint16_t w, h;
  on->setTextSize(size);
  on->getTextBounds(text, 0, 0, &bx, &by, &w, &h);
  on->setTextColor(colour);
  on->setCursor(cx - w / 2, cy - h / 2);
  on->print(text);
}

static bool buildOverlay() {
  overlay = (uint8_t *)ps_malloc((size_t)screenWidth * GAME_H);
  if (!overlay) return false;
  // Rasterised with Arduino_GFX into a throwaway canvas (no panel), using the
  // ids as colours, then turned into the column order the blit reads.
  Arduino_Canvas *mask = new Arduino_Canvas(screenWidth, GAME_H, nullptr);
  if (!mask->begin(GFX_SKIP_OUTPUT_BEGIN)) { delete mask; return false; }
  mask->fillScreen(0);
  const int16_t oy = -GAME_Y;   // screen y -> band y

  const int16_t half = DPAD_ARM_W / 2, len = DPAD_ARM_L - half;
  struct Arm { uint8_t id; int16_t x, y, w, h; } arms[4] = {
    { ID_UP,    (int16_t)(DPAD_X - half), (int16_t)(DPAD_Y - DPAD_ARM_L), DPAD_ARM_W, (int16_t)(len + 4) },
    { ID_DOWN,  (int16_t)(DPAD_X - half), (int16_t)(DPAD_Y + half - 4),   DPAD_ARM_W, (int16_t)(len + 4) },
    { ID_LEFT,  (int16_t)(DPAD_X - DPAD_ARM_L), (int16_t)(DPAD_Y - half), (int16_t)(len + 4), DPAD_ARM_W },
    { ID_RIGHT, (int16_t)(DPAD_X + half - 4),   (int16_t)(DPAD_Y - half), (int16_t)(len + 4), DPAD_ARM_W },
  };
  for (const Arm &arm : arms) {
    mask->fillRoundRect(arm.x, arm.y + oy, arm.w, arm.h, 8, arm.id);
    mask->drawRoundRect(arm.x, arm.y + oy, arm.w, arm.h, 8, ID_OUTLINE);
  }
  mask->fillRect(DPAD_X - half + 1, DPAD_Y - half + 1 + oy, DPAD_ARM_W - 2, DPAD_ARM_W - 2, ID_CENTRE);

  mask->fillCircle(BUTTON_A_X, BUTTON_A_Y + oy, BUTTON_R, ID_A);
  mask->drawCircle(BUTTON_A_X, BUTTON_A_Y + oy, BUTTON_R, ID_OUTLINE);
  centredTextOn(mask, "A", BUTTON_A_X, BUTTON_A_Y + oy, 3, ID_LABEL);
  mask->fillCircle(BUTTON_B_X, BUTTON_B_Y + oy, BUTTON_R, ID_B);
  mask->drawCircle(BUTTON_B_X, BUTTON_B_Y + oy, BUTTON_R, ID_OUTLINE);
  centredTextOn(mask, "B", BUTTON_B_X, BUTTON_B_Y + oy, 3, ID_LABEL);

  mask->fillRoundRect(SELECT_X, SMALL_Y + oy, SMALL_W, SMALL_H, 8, ID_SELECT);
  centredTextOn(mask, "SELECT", SELECT_X + SMALL_W / 2, SMALL_Y + SMALL_H / 2 + oy, 1, ID_LABEL);
  mask->fillRoundRect(START_X, SMALL_Y + oy, SMALL_W, SMALL_H, 8, ID_START);
  centredTextOn(mask, "START", START_X + SMALL_W / 2, SMALL_Y + SMALL_H / 2 + oy, 1, ID_LABEL);

  const uint16_t *pixels = mask->getFramebuffer();
  for (int16_t x = 0; x < screenWidth; x++) {
    columnHasOverlay[x] = 0;
    for (int16_t y = 0; y < GAME_H; y++) {
      uint8_t id = pixels[(int32_t)y * screenWidth + x] & 0x0F;
      overlay[(int32_t)x * GAME_H + y] = id;
      if (id) columnHasOverlay[x] = 1;
    }
  }
  delete mask;
  return true;
}

// Colours of every id for this frame: pressed controls lit.
static void buildColourTable(uint8_t pressed) {
  static const struct { uint8_t id; uint8_t button; } bodies[] = {
    { ID_UP, GB_BUTTON_UP }, { ID_DOWN, GB_BUTTON_DOWN }, { ID_LEFT, GB_BUTTON_LEFT }, { ID_RIGHT, GB_BUTTON_RIGHT },
    { ID_CENTRE, 0 }, { ID_A, GB_BUTTON_A }, { ID_B, GB_BUTTON_B },
    { ID_SELECT, GB_BUTTON_SELECT }, { ID_START, GB_BUTTON_START },
  };
  uint32_t base[5] = { paletteRgb[0], paletteRgb[1], paletteRgb[2], paletteRgb[3], 0x000000 };
  for (int id = 0; id < 16; id++)
    for (int b = 0; b < 5; b++) colourTable[id][b] = rgb565(base[b]);
  for (const auto &body : bodies) {
    uint32_t c = (pressed & body.button) ? pressedRgb : buttonRgb;
    for (int b = 0; b < 4; b++) colourTable[body.id][b] = rgb565(mixRgb(base[b], c, OVER_GAME_ALPHA));
    colourTable[body.id][4] = rgb565(c);
  }
  for (int b = 0; b < 5; b++) {
    colourTable[ID_OUTLINE][b] = rgb565(mixRgb(base[b], 0x000000, b < 4 ? 0.6f : 1.0f));
    colourTable[ID_LABEL][b] = rgb565(mixRgb(base[b], 0xFFFFFF, b < 4 ? LABEL_ALPHA : 1.0f));
  }
}

// A full-screen message; waits for a tap or a swipe.
static void showMessage(const char *title, const char *detail) {
  waitDisplayIdle();
  gfx->fillScreen(COLOR_BG);
  centredText(title, 240, 130, 2, COLOR_TEXT);
  if (detail) centredText(detail, 240, 170, 1, COLOR_DIM);
  centredText("Tap to go back to MokaBot", 240, 280, 1, COLOR_DIM);
  gfx->flush();
}

// ---- Display task (core 0) -------------------------------------------------

static TaskHandle_t displayTaskHandle = nullptr;
static volatile bool displayBusy = false;
static volatile uint8_t displayButtons = 0;   // controls to show lit

static void displayTask(void *param) {
  uint8_t tableButtons = 0xFF;
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t buttons = displayButtons;
    bool changed = buttons != tableButtons;
    if (changed) { buildColourTable(buttons); tableButtons = buttons; }
    displayGameBlit(frameShown, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, GAME_X, GAME_Y, overlay, columnHasOverlay,
                    colourTable, changed);
    gfx->flush();
    displayBusy = false;
  }
}

// Hands the finished frame to the display task, unless it is still sending
// the previous one (then this frame is not shown).
static bool showFrame(uint8_t buttons) {
  if (displayBusy) return false;
  memcpy(frameShown, frameWork, sizeof(frameShown));
  displayButtons = buttons;
  displayBusy = true;
  xTaskNotifyGive(displayTaskHandle);
  return true;
}

// Before drawing anything else (messages, leaving).
static void waitDisplayIdle() {
  while (displayBusy) vTaskDelay(1);
}

// ---- Leaving ---------------------------------------------------------------

static void leave() {
  waitDisplayIdle();
  gfx->fillScreen(COLOR_BG);
  centredText("Back to MokaBot...", 240, 160, 2, COLOR_TEXT);
  gfx->flush();
  SD.end();
  delay(300);
  ESP.restart();
}

static void waitTapThenLeave() {
  bool wasDown = false;
  while (true) {
    readTouches();
    if (touches.count > 0) wasDown = true;
    else if (wasDown) leave();
    if (getMokaStatus() != 0) setMokaStatus(0);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---- Game list -------------------------------------------------------------

static String roms[MAX_ROMS];
static size_t romSizes[MAX_ROMS];   // from the folder listing
static int romCount = 0;

static bool isRomName(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".gb") && !lower.startsWith(".");
}

static void scanRoms() {
  romCount = 0;
  File root = SD.open("/");
  if (!root) return;
  for (File entry = root.openNextFile(); entry && romCount < MAX_ROMS; entry = root.openNextFile()) {
    String name = entry.name();
    Serial.printf("Game Boy: SD entry \"%s\" (path \"%s\"), %s, %u bytes\n", name.c_str(), entry.path(),
                  entry.isDirectory() ? "folder" : "file", (unsigned)entry.size());
    if (!entry.isDirectory() && isRomName(name)) {
      // The full path as the card reports it.
      String path = entry.path();
      if (!path.startsWith("/")) path = "/" + path;
      romSizes[romCount] = entry.size();
      roms[romCount++] = path;
    }
    entry.close();
  }
  root.close();
  // Alphabetical.
  for (int a = 0; a < romCount; a++)
    for (int b = a + 1; b < romCount; b++)
      if (roms[b].compareTo(roms[a]) < 0) {
        String t = roms[a]; roms[a] = roms[b]; roms[b] = t;
        size_t z = romSizes[a]; romSizes[a] = romSizes[b]; romSizes[b] = z;
      }
}

#define LIST_ROWS     6
#define LIST_TOP      56
#define LIST_ROW_H    38

static void drawList(int page) {
  gfx->fillScreen(COLOR_BG);
  centredText("Game Boy", 240, 18, 2, COLOR_TEXT);
  centredText("Tap a game. Swipe up to go back to MokaBot.", 240, 40, 1, COLOR_DIM);
  for (int row = 0; row < LIST_ROWS; row++) {
    int index = page * LIST_ROWS + row;
    if (index >= romCount) break;
    int y = LIST_TOP + row * LIST_ROW_H;
    gfx->fillRoundRect(60, y, 360, LIST_ROW_H - 6, 6, COLOR_BUTTON);
    String name = roms[index].substring(1);
    name = name.substring(0, name.length() - 3);
    if (name.length() > 28) name = name.substring(0, 27) + "~";
    gfx->setTextSize(2);
    gfx->setTextColor(COLOR_TEXT);
    gfx->setCursor(72, y + 9);
    gfx->print(name);
  }
  int pages = (romCount + LIST_ROWS - 1) / LIST_ROWS;
  if (pages > 1) {
    char text[16];
    snprintf(text, sizeof(text), "%d / %d", page + 1, pages);
    centredText(text, 240, 300, 1, COLOR_DIM);
    if (page > 0) { gfx->fillRoundRect(4, 130, 48, 60, 8, COLOR_BUTTON); centredText("<", 28, 160, 3, COLOR_LABEL); }
    if (page < pages - 1) { gfx->fillRoundRect(428, 130, 48, 60, 8, COLOR_BUTTON); centredText(">", 452, 160, 3, COLOR_LABEL); }
  }
  gfx->flush();
}

// Returns the chosen ROM; a swipe up leaves.
static int chooseRom() {
  int page = 0;
  int pages = (romCount + LIST_ROWS - 1) / LIST_ROWS;
  drawList(page);
  bool down = false;
  int16_t startX = 0, startY = 0, lastX = 0, lastY = 0;
  while (true) {
    readTouches();
    if (getMokaStatus() != 0) setMokaStatus(0);
    if (touches.count > 0) {
      if (!down) { down = true; startX = touches.x[0]; startY = touches.y[0]; }
      lastX = touches.x[0]; lastY = touches.y[0];
    }
    else if (down) {
      down = false;
      if (startY - lastY > EXIT_SWIPE) leave();
      // A tap: where it started.
      if (abs(lastX - startX) < 30 && abs(lastY - startY) < 30) {
        if (startX < 56 && page > 0) { drawList(--page); }
        else if (startX >= 424 && page < pages - 1) { drawList(++page); }
        else if (startX >= 60 && startX < 420 && startY >= LIST_TOP) {
          int row = (startY - LIST_TOP) / LIST_ROW_H;
          int index = page * LIST_ROWS + row;
          if (row < LIST_ROWS && index < romCount) return index;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---- Running a game --------------------------------------------------------

static uint8_t *rom = nullptr;
static uint8_t *cartRam = nullptr;
static size_t cartRamSize = 0;
static String savePath;

// Why the last loadRom() failed, for the message on the screen.
static char loadError[96] = "";

static bool loadRom(const String &path, size_t listedSize, size_t *size) {
  File file = SD.open(path, FILE_READ);
  if (!file) {
    snprintf(loadError, sizeof(loadError), "Could not open %s", path.c_str());
    return false;
  }
  size_t length = file.size();
  Serial.printf("Game Boy: opened \"%s\": size %u, listed %u, available %d\n", path.c_str(),
                (unsigned)length, (unsigned)listedSize, file.available());
  // Some cards report 0 for an opened file: use the folder listing's size.
  if (length == 0) length = listedSize;
  if (length < 0x150 || length > MAX_ROM_BYTES) {
    snprintf(loadError, sizeof(loadError), "Wrong size for a Game Boy ROM: %u bytes", (unsigned)length);
    file.close();
    return false;
  }
  rom = (uint8_t *)ps_malloc(length);
  if (!rom) {
    snprintf(loadError, sizeof(loadError), "Not enough memory: %u KB needed, PSRAM %u KB free",
             (unsigned)(length / 1024), (unsigned)(ESP.getFreePsram() / 1024));
    file.close();
    return false;
  }
  size_t done = 0;
  while (done < length) {
    int n = file.read(rom + done, min((size_t)32768, length - done));
    if (n <= 0) break;
    done += n;
  }
  file.close();
  *size = done;
  if (done != length) {
    snprintf(loadError, sizeof(loadError), "SD read stopped at %u of %u bytes", (unsigned)done, (unsigned)length);
    return false;
  }
  return true;
}

static void loadSave() {
  File file = SD.open(savePath);
  if (!file) return;
  if (file.size() == cartRamSize) file.read(cartRam, cartRamSize);
  file.close();
}

static void writeSave() {
  if (!cartRam || !cartRamSize) return;
  File file = SD.open(savePath, FILE_WRITE);
  if (!file) return;
  file.write(cartRam, cartRamSize);
  file.close();
}

static void runGame(int index) {
  gfx->fillScreen(COLOR_BG);
  centredText("Loading...", 240, 160, 2, COLOR_TEXT);
  gfx->flush();

  size_t romSize = 0;
  if (!loadRom(roms[index], romSizes[index], &romSize)) {
    Serial.printf("Game Boy: %s\n", loadError);
    showMessage("Could not load the game", loadError);
    waitTapThenLeave();
  }
  int error = gbcoreInit(rom, romSize);
  if (error) {
    showMessage("This game is not supported", error == 1 ? "Unsupported cartridge type." : "Bad ROM header checksum.");
    waitTapThenLeave();
  }
  long saveSize = gbcoreSaveSize();
  if (saveSize > 0) {
    cartRamSize = saveSize;
    cartRam = (uint8_t *)ps_calloc(cartRamSize, 1);
    if (cartRam) {
      gbcoreSetCartRam(cartRam, cartRamSize);
      savePath = roms[index].substring(0, roms[index].length() - 3) + ".sav";
      loadSave();
    }
    else cartRamSize = 0;
  }
  gbcoreTakeRamDirty();

  if (!buildOverlay()) {
    showMessage("Not enough memory", "For the on-screen controls.");
    waitTapThenLeave();
  }
  gfx->fillScreen(COLOR_BG);
  char title[17];
  gbcoreRomName(title);
  centredText(title, 240, GAME_Y + GAME_H + 8, 1, COLOR_DIM);

  gfx->flush();
  xTaskCreatePinnedToCore(displayTask, "gbdisplay", 4096, NULL, 3, &displayTaskHandle, 0);

  uint32_t nextFrame = micros();
  uint32_t lastSaved = millis();
  bool saveDirty = false;
  uint8_t behindFrames = 0;
  // A finger that went down on the game picture, for the exit swipe.
  bool swipe = false;
  int16_t swipeStartY = 0;

  // Timing log, every 2 s (serial): emulated and shown frames per second and
  // where the time goes.
  uint32_t statStart = millis(), statFrames = 0, statShown = 0;
  uint64_t statTouchUs = 0, statRunUs = 0, statSleepUs = 0;

  while (true) {
    if (getMokaStatus() != 0) setMokaStatus(0);   // nothing heats while playing
    uint32_t t0 = micros();
    readTouches();
    statTouchUs += micros() - t0;
    uint8_t buttons = 0;
    for (int k = 0; k < touches.count; k++) buttons |= buttonsAt(touches.x[k], touches.y[k]);
    gbcoreSetButtons(buttons);

    if (touches.count == 1 && inGame(touches.x[0], touches.y[0])) {
      if (!swipe) { swipe = true; swipeStartY = touches.y[0]; }
      else if (swipeStartY - touches.y[0] > EXIT_SWIPE) {
        if (gbcoreTakeRamDirty() || saveDirty) writeSave();
        leave();
      }
    }
    else if (touches.count == 0) swipe = false;

    if (gbcoreFailed()) {
      if (saveDirty || gbcoreTakeRamDirty()) writeSave();
      showMessage("The game stopped", "The emulator hit an instruction it cannot run.");
      waitTapThenLeave();
    }

    t0 = micros();
    gbcoreRunFrame(true);
    statRunUs += micros() - t0;
    statFrames++;
    if (showFrame(buttons)) statShown++;

    if (gbcoreTakeRamDirty()) saveDirty = true;
    if (saveDirty && millis() - lastSaved > SAVE_EVERY_MS) {
      waitDisplayIdle();   // the SD card and the panel are on different buses, but keep it simple
      writeSave();
      saveDirty = false;
      lastSaved = millis();
    }

    if (millis() - statStart >= 2000) {
      float seconds = (millis() - statStart) / 1000.0f;
      Serial.printf("Game Boy: %.1f frames/s emulated (59.7 = full speed), %.1f shown; per frame: run %.1f ms, "
                    "touch %.1f ms, sleep %.1f ms\n",
                    statFrames / seconds, statShown / seconds, statRunUs / 1000.0f / statFrames,
                    statTouchUs / 1000.0f / statFrames, statSleepUs / 1000.0f / statFrames);
      statStart = millis(); statFrames = statShown = 0;
      statTouchUs = statRunUs = statSleepUs = 0;
    }

    // Keep 59.73 frames/s. When behind, only yield every few frames; far
    // behind (an SD write), start over from now.
    nextFrame += FRAME_US;
    int32_t wait = (int32_t)(nextFrame - micros());
    t0 = micros();
    if (wait > 1000) { vTaskDelay(pdMS_TO_TICKS(wait / 1000)); behindFrames = 0; }
    else {
      if (++behindFrames >= 8) { vTaskDelay(1); behindFrames = 0; }
      if (wait < -100000) nextFrame = micros();
    }
    statSleepUs += micros() - t0;
  }
}

static void gameTask(void *param) {
  setMokaStatus(0);
  Serial.printf("Game Boy: PSRAM %s, %u KB free; internal %u KB free\n", psramFound() ? "found" : "NOT FOUND",
                (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getFreeHeap() / 1024));
  // LVGL never draws again: the board restarts when the player leaves.
  xSemaphoreTake(lvglMutex, portMAX_DELAY);
  gfx = displayGameBegin();
  setColours();

  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSpi, SD_HZ)) {
    showMessage("No SD card", "Insert a FAT32 card with .gb files in its main folder.");
    waitTapThenLeave();
  }
  scanRoms();
  if (romCount == 0) {
    showMessage("No games found", "Put .gb files in the main folder of the SD card.");
    waitTapThenLeave();
  }
  runGame(chooseRom());
}

extern "C" void gameboyOpen(void) {
  if (running) return;
  running = true;
  // Its own task: called from an LVGL event, which holds the LVGL lock.
  if (xTaskCreatePinnedToCore(gameTask, "gameboy", 8192, NULL, 3, NULL, CORE_UI) != pdPASS) running = false;
}

#endif // MOKA_GAMEBOY
