#pragma once

// Brew history: one sample per second of the current (or last) extraction
// (status 2) or manual power run (status 3), kept on the device so the brew
// page can load the whole curve at once and then fetch only new samples.
// The last brew stays available until the next one starts, which clears it.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the recording task. Call once from setup().
void brewHistoryStart(void);

// Pressure inside the moka (bar), estimated from the temperature once it is
// above the boiling point; the ambient pressure below it.
float brewInternalPressure(void);

// Saved slots, index 0..2 = 'A'..'C'. Cached in memory (read at startup and
// after each save), so the UI never touches the filesystem.
bool brewSlotPresent(int index);
int brewSlotSeconds(int index);   // duration of the saved brew
int brewSlotMode(int index);      // 2 extraction, 3 manual power

#ifdef __cplusplus
} // extern "C"

#include <Arduino.h>
// JSON for GET /brewHistory?session=S&from=N:
// {"session":S,"mode":2|3,"active":true|false,"next":M,
//  "samples":[[seconds,tempC|null,pressureBar|null,targetC|null],...]}
// Pressure and target are recorded for extractions only (null in manual
// power, which has no PID).
// Samples start at index N when S is the current session, otherwise at the
// oldest one kept (the client starts over).
String brewHistoryJson(long session, long from);

// ---- Saved brews: slots A, B and C (files in the SPIFFS partition) ----
// Result of brewHistorySaveSlot().
enum BrewSlotResult {
  BREW_SLOT_SAVED = 0,
  BREW_SLOT_INVALID,     // slot is not A, B or C
  BREW_SLOT_NOT_OFF,     // the moka is not OFF (status 0)
  BREW_SLOT_EMPTY,       // no finished brew recorded
  BREW_SLOT_FS_ERROR     // filesystem / memory error
};
// Saves the last recorded brew into slot 'A', 'B' or 'C' (overwriting it).
// Only allowed while the moka is OFF, i.e. the brew has finished.
BrewSlotResult brewHistorySaveSlot(char slot);
// Deletes the brew saved in slot 'A', 'B' or 'C'. Returns BREW_SLOT_SAVED on
// success (also when the slot was already empty), BREW_SLOT_INVALID or
// BREW_SLOT_FS_ERROR otherwise.
BrewSlotResult brewHistoryDeleteSlot(char slot);
// {"A":{"samples":n,"seconds":s,"mode":2|3}|null,"B":...,"C":...}
String brewSlotsJson();
#endif
