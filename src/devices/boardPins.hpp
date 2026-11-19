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

#elif defined(BOARD_JC3248W535)

// Guition JC3248W535. Its connectors: P2 has GPIO 5 (battery divider), 6, 7,
// 15, 16, 46 (strapping pin), 9 and 14; P3 and P4 (QWIIC) both have GPIO 17
// and 18 with 3V3 and GND.
// Heater relay on P2. The pin floats until the firmware starts: with a relay
// module that switches on HIGH, fit a 10 k pull-down to GND so it stays off
// during reset and flashing.
#define PIN_HEATER      14
// Sensors on P3 / P4, a bus of their own: the touch controller's bus
// (GPIO 4 / 8) is not on a connector.
#define PIN_SENSOR_SDA  17
#define PIN_SENSOR_SCL  18

#elif defined(BOARD_WT32_SC01)

#define PIN_HEATER      32
// Sensors share the touch controller's I2C bus (GPIO 18/19).
// the display's DC and reset lines and must not be used for I2C.
#define PIN_SENSOR_SDA  18
#define PIN_SENSOR_SCL  19

#else
#error "No BOARD_* build flag: add the board to boardPins.hpp"
#endif
