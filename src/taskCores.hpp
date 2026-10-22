#pragma once

// Core assignment of the FreeRTOS tasks (ESP32 / ESP32-S3 are dual core).
// Everything that talks on the shared I2C bus (touch, ADS1115, BMP280 and the
// PID task, which reads the BMP280) runs on CORE_I2C, so bus transactions are
// never contended from both cores. LVGL drawing and UI logic run alone on
// CORE_UI, so rendering does not delay I2C traffic and vice versa.
#define CORE_I2C 0
#define CORE_UI  1
