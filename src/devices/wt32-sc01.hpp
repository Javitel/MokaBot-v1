#pragma once

// Display + touch driver for the WT32-SC01 (ST7796 panel, FT5x06 touch) and
// its LVGL glue. Everything hardware specific lives in wt32-sc01.cpp.
// Compiled only when BOARD_WT32_SC01 is defined.

#define screenWidth 480
#define screenHeight 320

// Starts the panel, backlight and touch, initialises LVGL and registers the
// LVGL display and touch input drivers. Call once from setup() before ui_init().
void displayInit();
