#pragma once

// Short key-press beeps. Implemented by boards with audio output (currently
// the Waveshare ESP32-S3-Touch-LCD-3.5B, through its ES8311 codec); a no-op on
// the others. C-compatible so the LVGL UI code can call it.

#ifdef __cplusplus
extern "C" {
#endif

#if defined(BOARD_WAVESHARE_S3_TOUCH_LCD_35B)
// Sets up the codec and I2S; call once after displayInit() (needs Wire).
void beepInit(void);
// Non-blocking: queue a single / double beep for the audio task.
void beep(void);
void beepDouble(void);
#else
static inline void beepInit(void) {}
static inline void beep(void) {}
static inline void beepDouble(void) {}
#endif

#ifdef __cplusplus
}
#endif
