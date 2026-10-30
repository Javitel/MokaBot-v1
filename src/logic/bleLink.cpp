#include "bleLink.hpp"

#ifndef MOKA_BLE

void bleStart(void) {}
void bleSetName(const char *name) {}
bool bleConnected(void) { return false; }

#else

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "logic/api.hpp"
#include "logic/webserver.hpp"
#include "devices/beep.hpp"
#include "taskCores.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define BLE_LAST_PACKET     0x01
// Largest request accepted (a brew import is ~20 KB of text).
#define BLE_MAX_REQUEST     32768
// A notification that does not fit in the stack's buffers is retried.
#define BLE_NOTIFY_RETRIES  200
#define BLE_NOTIFY_RETRY_MS 5

static NimBLEServer *bleServer = nullptr;
static NimBLECharacteristic *txCharacteristic = nullptr;
static volatile bool connected = false;
static volatile uint16_t peerMtu = 23;

// Request being received (written by the NimBLE host task) and complete
// requests waiting for the worker task (as heap-allocated Strings).
static String rxBuffer;
static bool rxOverflow = false;
static QueueHandle_t requestQueue = nullptr;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    connected = true;
    peerMtu = 23;
    rxBuffer = "";
    rxOverflow = false;
    // Tells which MokaBot the phone connected to (boards with audio; the
    // screen's blue square flashes too).
    beep();
    Serial.println("Bluetooth: phone connected");
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    connected = false;
    Serial.printf("Bluetooth: phone disconnected (%d)\n", reason);
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &connInfo) override {
    peerMtu = mtu;
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    NimBLEAttValue value = characteristic->getValue();
    if (value.length() == 0) return;
    const uint8_t *data = value.data();
    if (!rxOverflow) {
      if (rxBuffer.length() + value.length() - 1 > BLE_MAX_REQUEST) rxOverflow = true;
      else rxBuffer.concat((const char *)data + 1, value.length() - 1);
    }
    if (data[0] & BLE_LAST_PACKET) {
      // An oversized request is answered as an empty one (the worker replies
      // with an error instead of the client waiting forever).
      String *request = new String(rxOverflow ? String("TOOBIG") : rxBuffer);
      rxBuffer = "";
      rxOverflow = false;
      if (xQueueSend(requestQueue, &request, 0) != pdTRUE) delete request;
    }
  }
};

// Sends one message as packets of at most (MTU - 3) bytes.
static void sendMessage(const String &message) {
  size_t chunk = peerMtu > 4 ? peerMtu - 3 - 1 : 19;
  uint8_t *packet = (uint8_t *)malloc(chunk + 1);
  if (!packet) return;
  size_t offset = 0;
  do {
    size_t length = message.length() - offset;
    if (length > chunk) length = chunk;
    bool last = offset + length >= message.length();
    packet[0] = last ? BLE_LAST_PACKET : 0;
    memcpy(packet + 1, message.c_str() + offset, length);
    bool sent = false;
    for (int attempt = 0; attempt < BLE_NOTIFY_RETRIES && connected; attempt++) {
      if (txCharacteristic->notify(packet, length + 1)) { sent = true; break; }
      vTaskDelay(pdMS_TO_TICKS(BLE_NOTIFY_RETRY_MS));
    }
    if (!sent) break;   // disconnected or stuck
    offset += length;
  } while (offset < message.length());
  free(packet);
}

static void sendResponse(int status, const char *type, const String &body) {
  String message = String(status) + " " + type + "\n";
  message += body;
  sendMessage(message);
}

// "<METHOD> <path>[?<query>]\n<body>" -> apiHandle().
static void handleRequest(const String &request) {
  if (request == "TOOBIG") { sendResponse(413, "text/plain", "Request too large"); return; }
  int lineEnd = request.indexOf('\n');
  String line = lineEnd < 0 ? request : request.substring(0, lineEnd);
  String body = lineEnd < 0 ? String() : request.substring(lineEnd + 1);
  int space = line.indexOf(' ');
  if (space < 0) { sendResponse(400, "text/plain", "Bad request"); return; }
  String method = line.substring(0, space);
  String target = line.substring(space + 1);
  int question = target.indexOf('?');
  String path = question < 0 ? target : target.substring(0, question);
  ApiQueryArgs args(question < 0 ? String() : target.substring(question + 1));

  ApiResponse response;
  if (apiHandle(method, path, args, body, response)) sendResponse(response.status, response.contentType, response.body);
  else sendResponse(404, "text/plain", "Not found");
}

// Answers the requests one by one, away from the NimBLE host task.
static void bleWorker(void *param) {
  String *request;
  while (true) {
    if (xQueueReceive(requestQueue, &request, portMAX_DELAY) == pdTRUE) {
      if (connected) handleRequest(*request);
      delete request;
    }
  }
}

// Advertises the service and, in the scan response (room for up to 29
// characters), the device name, so the app can list several MokaBots by name.
static void applyName(const char *name) {
  NimBLEDevice::setDeviceName(name);
  NimBLEAdvertisementData advertisementData;
  advertisementData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advertisementData.addServiceUUID(MOKA_BLE_SERVICE_UUID);
  NimBLEAdvertisementData scanResponse;
  scanResponse.setName(name);
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(advertisementData);
  advertising->setScanResponseData(scanResponse);
  advertising->enableScanResponse(true);
}

void bleSetName(const char *name) {
  if (!bleServer) return;
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  bool wasAdvertising = advertising->isAdvertising();
  if (wasAdvertising) advertising->stop();
  applyName(name);
  // While connected the stack does not advertise; onDisconnect() restarts it
  // with the new name.
  if (wasAdvertising) advertising->start();
}

void bleStart(void) {
  if (bleServer) return;
  requestQueue = xQueueCreate(4, sizeof(String *));
  NimBLEDevice::init(getDeviceName());
  NimBLEDevice::setMTU(517);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService *service = bleServer->createService(MOKA_BLE_SERVICE_UUID);
  NimBLECharacteristic *rx = service->createCharacteristic(
    MOKA_BLE_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  txCharacteristic = service->createCharacteristic(MOKA_BLE_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  service->start();

  applyName(getDeviceName());
  NimBLEDevice::getAdvertising()->start();

  // Like the web server: no I2C, next to the radio stacks on core 0. 8 KB:
  // the JSON answers are built on the heap, but SPIFFS calls need stack.
  xTaskCreatePinnedToCore(bleWorker, "ble", 8192, NULL, 3, NULL, CORE_I2C);
  Serial.printf("Bluetooth: advertising as %s\n", getDeviceName());
}

bool bleConnected(void) {
  return connected;
}

#endif
