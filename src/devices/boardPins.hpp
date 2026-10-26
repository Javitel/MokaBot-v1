#pragma once

// Timeout of each I2C transaction on the shared bus (Arduino default: 50 ms).
#define I2C_TIMEOUT_MS  200

// Moka hardware pins (heater relay, sensor I2C bus) per board, selected with
// the BOARD_* build flag in platformio.ini. Display/touch pins live in the
// board's driver file in this folder.

#if defined(BOARD_WAVESHARE_S3_TOUCH_LCD_35B)

// GPIO17 is the camera VSYNC line, free when no camera is attached. The board
// uses GPIO 1-16, 19-21, 38-48 for LCD, I2C, SD, audio, USB and camera.
#define PIN_HEATER      17
// Sensors share the board's I2C bus (also used by touch, IO expander, PMU...).
#define PIN_SENSOR_SDA  8
#define PIN_SENSOR_SCL  7

#else // BOARD_WT32_SC01

#define PIN_HEATER      32
// Sensors share the touch controller's I2C bus (GPIO 18/19).
// the display's DC and reset lines and must not be used for I2C.
#define PIN_SENSOR_SDA  18
#define PIN_SENSOR_SCL  19

#endif
