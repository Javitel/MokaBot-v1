#pragma once

// Serialises access to Preferences (NVS) across tasks. A Preferences object
// is not thread safe, and several tasks (UI, web server, calibration, OTA)
// read and write settings, so every begin() ... end() block runs with this
// lock held: no task can read or write while another one is using the store.
// Recursive, so a task that already holds it can take it again.

#ifdef __cplusplus
extern "C" {
#endif

void prefsLock(void);
void prefsUnlock(void);

#ifdef __cplusplus
}
#endif
