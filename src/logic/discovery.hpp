#pragma once

// WiFi discovery for the Android app's "Search": the app broadcasts the text
// "MOKABOT?" to UDP port MOKA_DISCOVERY_PORT, and every MokaBot on the network
// replies to the sender with {"name":"<device name>","ip":"a.b.c.d"}.

#define MOKA_DISCOVERY_PORT 41820

// Listens on MOKA_DISCOVERY_PORT. False if the socket could not be set up.
bool discoveryStart(void);
// Answers the requests received since the last call (never blocks).
void discoveryPoll(void);
void discoveryStop(void);
