#pragma once

// Brew history: one sample per second of the current (or last) extraction
// (status 2) or manual power run (status 3), kept on the device so the brew
// page can load the whole curve at once and then fetch only new samples.

#ifdef __cplusplus
extern "C" {
#endif

// Starts the recording task. Call once from setup().
void brewHistoryStart(void);

// Pressure inside the moka (bar), estimated from the temperature once it is
// above the boiling point; the ambient pressure below it.
float brewInternalPressure(void);

#ifdef __cplusplus
} // extern "C"

#include <Arduino.h>
// JSON for GET /brewHistory?session=S&from=N:
// {"session":S,"mode":2|3,"active":true|false,"next":M,
//  "samples":[[seconds,tempC|null,pressureBar|null],...]}
// Samples start at index N when S is the current session, otherwise at the
// oldest one kept (the client starts over).
String brewHistoryJson(long session, long from);
#endif
