#pragma once

// OTA (over-the-air) firmware update client. Downloads this board's firmware
// file (otaFirmwareFile()) from a configurable HTTP(S) base URL (a folder),
// flashes it and reboots. The base URL is stored in Preferences and can be
// changed from the settings web page.
//
// The current firmware version is sent in the x-ESP32-version header, so a
// server can answer 304 (Not Modified) when there is nothing newer.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts a check/update in the background (no-op if one is running).
// Switches the moka off before flashing; reboots when the update succeeds.
void otaStartCheck(void);
// True while an update runs, and for a few seconds after it ends so its
// result stays readable on screen.
bool otaInProgress(void);
// Short status for the screen, e.g. "OTA: 42%" or "OTA: up to date".
const char * otaStatusText(void);
// Download progress 0-100, or -1 when no download is in progress.
int otaProgress(void);

#ifdef __cplusplus
} // extern "C"

#include <Arduino.h>
// Base URL (folder) of the firmware files.
String otaGetUrl();
void otaSetUrl(const String &url);
// File name this board downloads from the base URL.
const char * otaFirmwareFile();
#endif
