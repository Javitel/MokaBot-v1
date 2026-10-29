#pragma once

// Bluetooth LE link for the Android app (boards built with MOKA_BLE, i.e. the
// Waveshare ESP32-S3; the WT32-SC01 has no IRAM left for Bluetooth and only
// offers WiFi). It carries the same requests as the web server's data
// endpoints (see api.hpp), so the app's pages work over either transport.
//
// GATT service MOKA_BLE_SERVICE_UUID, advertised as "MokaBot":
//  - MOKA_BLE_RX_UUID (write / write without response): requests
//  - MOKA_BLE_TX_UUID (notify): responses
// A message is sent as packets of at most (MTU - 3) bytes; the first byte of
// each packet is a flag (bit 0 set = last packet of the message), the rest is
// payload. Messages:
//   request:  "<METHOD> <path>[?<query>]\n<body>"     e.g. "GET /brewSlots\n"
//   response: "<status> <content type>\n<body>"       e.g. "200 application/json\n{...}"
// One request at a time: the app waits for each response before the next.

#define MOKA_BLE_SERVICE_UUID "6d6f6b61-626f-7400-0000-000000000001"
#define MOKA_BLE_RX_UUID      "6d6f6b61-626f-7400-0000-000000000002"
#define MOKA_BLE_TX_UUID      "6d6f6b61-626f-7400-0000-000000000003"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// Starts advertising and serving requests. No-op without MOKA_BLE.
void bleStart(void);
// True while a phone is connected over Bluetooth (always false without
// MOKA_BLE).
bool bleConnected(void);

#ifdef __cplusplus
}
#endif
