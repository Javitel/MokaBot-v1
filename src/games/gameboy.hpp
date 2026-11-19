#pragma once

// Game Boy easter egg: swipe down on the main screen to open it. Boards built
// with MOKA_GAMEBOY only (an SD card slot and PSRAM); a no-op elsewhere.
// C-compatible so the LVGL UI code can call it.

#ifdef __cplusplus
extern "C" {
#endif

#if defined(MOKA_GAMEBOY)
// Switches the moka OFF and starts the game list (on its own task). The
// board restarts into MokaBot when the player swipes up to leave.
void gameboyOpen(void);
#else
static inline void gameboyOpen(void) {}
#endif

#ifdef __cplusplus
}
#endif
