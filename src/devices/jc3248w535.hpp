#pragma once

// Display + touch driver for the Guition JC3248W535 (ESP32-S3, 3.5" AXS15231B
// panel over QSPI with integrated capacitive touch) and its LVGL glue.
// Everything hardware specific lives in jc3248w535.cpp.
// Compiled only when BOARD_JC3248W535 is defined.
//
// Requires the "moononournation/GFX Library for Arduino" library (Arduino_GFX).

#define screenWidth 480
#define screenHeight 320

// Starts the display, backlight and touch, initialises LVGL and registers
// the LVGL display and touch input drivers. Call once from setup() before
// ui_init(). Also starts Wire on the sensor pins (boardPins.hpp); the touch
// controller is on its own bus (Wire1).
void displayInit();

#ifdef __cplusplus
class Arduino_GFX;
// Game Boy easter egg (src/games/gameboy.cpp): stops the LVGL touch task and
// returns the full-frame canvas (480x320 landscape) to draw on directly;
// flush() pushes it to the panel.
Arduino_GFX *displayGameBegin();
// Draws the screen band from row gy, srcH * 2 rows high and the full screen
// width, into the canvas (flush() then shows it): a srcW x srcH picture of
// 2-bit shades scaled x2 with its left edge at column gameX, black around it,
// and an overlay on top. overlay holds one id (0-15) per pixel of the band,
// column by column (screenWidth columns of srcH * 2); the pixel's colour is
// colors[id][base], base being the game shade (0-3) or 4 outside the game.
// columnHasOverlay[x] (screenWidth flags) is false for columns with id 0
// only: their mask is not read. Columns outside the game are only drawn when
// redrawAll is set (the first frame, and when the colours changed).
void displayGameBlit(const uint8_t *shades, int16_t srcW, int16_t srcH, int16_t gameX, int16_t gy,
                     const uint8_t *overlay, const uint8_t *columnHasOverlay,
                     const uint16_t (*colors)[5], bool redrawAll);
// Reads the touch controller now: up to max points in screen coordinates.
// Returns the number of fingers (0: released), or -1 when the controller had
// no new data (keep the previous state).
int displayReadTouchPoints(int16_t *xs, int16_t *ys, int max);
#endif
