#pragma once

// Display + touch driver for the Waveshare ESP32-S3-Touch-LCD-3.5B
// (AXS15231B panel over QSPI with integrated touch) and its LVGL glue.
// Everything hardware specific lives in waveshare-s3-touch-lcd-3.5b.cpp.
// Compiled only when BOARD_WAVESHARE_S3_TOUCH_LCD_35B is defined.
//
// Requires the "moononournation/GFX Library for Arduino" library (Arduino_GFX).

#define screenWidth 480
#define screenHeight 320

// Resets the panel, starts the display, backlight and touch, initialises LVGL
// and registers the LVGL display and touch input drivers. Call once from
// setup() before ui_init(). Also starts Wire on the board's I2C pins.
void displayInit();
