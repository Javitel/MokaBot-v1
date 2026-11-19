#pragma once

// Short key-press beeps. Implemented by boards with audio output (the
// Waveshare ESP32-S3-Touch-LCD-3.5B, through its ES8311 codec, and the Guition
// JC3248W535, through its I2S amplifier); a no-op on the others.
// C-compatible so the LVGL UI code can call it.

#ifdef __cplusplus
extern "C" {
#endif

#if defined(BOARD_WAVESHARE_S3_TOUCH_LCD_35B) || defined(BOARD_JC3248W535)
// Sets up the audio output; call once after displayInit().
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
