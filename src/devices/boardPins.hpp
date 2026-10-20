#pragma once

// Moka hardware pins (heater relay, sensor I2C bus) per board, selected with
// the BOARD_* build flag in platformio.ini. Display/touch pins live in the
// board's driver file in this folder.

#if defined(BOARD_WAVESHARE_S3_TOUCH_LCD_35B)

// GPIO17 is the camera VSYNC line, free when no camera is attached. The board
// uses GPIO 1-16, 19-21, 38-48 for LCD, I2C, SD, audio, USB and camera.
#warning "Waveshare 3.5B: PIN_HEATER is a placeholder, set it to the GPIO the relay is wired to"
#define PIN_HEATER      17
// Sensors share the board's I2C bus (also used by touch, IO expander, PMU...).
#define PIN_SENSOR_SDA  8
#define PIN_SENSOR_SCL  7

#else // BOARD_WT32_SC01

#define PIN_HEATER      32
#define PIN_SENSOR_SDA  21
#define PIN_SENSOR_SCL  22

#endif
