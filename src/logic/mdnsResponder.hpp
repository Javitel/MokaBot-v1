#pragma once

// Minimal mDNS responder: answers address queries for "<hostname>.local", so
// http://mokabot.local works, without the ESP-IDF mdns component (~30 KB of
// flash). It only answers A (and ANY) questions for that one name: no service
// advertising (_http._tcp), no probing for name conflicts, no IPv6.
//
// Replies go to the mDNS group (224.0.0.251:5353); "legacy" queries sent from
// another port (one-shot resolvers) get a unicast reply with their question
// and ID, as RFC 6762 section 6.7 asks.

#include <stdint.h>

// Joins the mDNS group on the interface with address ip (4 bytes, a.b.c.d)
// and announces the name once. Returns false if the socket could not be set up.
bool mdnsResponderStart(const char *hostname, const uint8_t ip[4]);
// Answers the queries received since the last call; call it often (it never
// blocks).
void mdnsResponderPoll(void);
void mdnsResponderStop(void);
