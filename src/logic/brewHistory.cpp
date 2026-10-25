#include "brewHistory.hpp"
#include "logic/moka.hpp"
#include "logic/pressureSensor.hpp"
#include "taskCores.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <SPIFFS.h>

#define BREW_HISTORY_CAPACITY    900   // 15 min at one sample per second
#define BREW_HISTORY_PERIOD_MS   1000
#define STATUS_BREWING           2
#define STATUS_MANUAL_POWER      3

// Compact sample: 8 bytes, 900 samples ~ 7.2 KB.
typedef struct {
  uint16_t seconds;        // since the session started
  int16_t  tempDeci;       // 0.1 C; INT16_MIN = no valid reading
  uint16_t pressureMbar;   // 0 = not recorded (manual power)
  int16_t  targetDeci;     // PID target, 0.1 C; INT16_MIN = none (manual power)
} BrewSample;

static BrewSample samples[BREW_HISTORY_CAPACITY];
static long totalSamples = 0;    // samples recorded this session (index of the next one)
static long sessionId = 0;       // incremented at each new session
static int sessionMode = 0;      // 2 or 3 (0 before the first session)
static bool sessionActive = false;
static uint32_t sessionStartMs = 0;
static SemaphoreHandle_t historyMutex = NULL;

// ---- Saved brew slots ----
// One file per slot in the SPIFFS partition (unused since the web pages are
// embedded in the firmware; NVS is too small for three brews):
//   SlotHeader + count x BrewSample
#define BREW_SLOT_MAGIC 0x31424B4D   // "MKB1"
#define BREW_SLOT_FIRST 'A'
#define BREW_SLOT_LAST  'C'

typedef struct {
  uint32_t magic;
  uint8_t  mode;       // 2 extraction, 3 manual power
  uint8_t  reserved;
  uint16_t count;      // samples that follow
} SlotHeader;

static bool slotsFsReady = false;

// What each slot holds, cached for the UI (see refreshSlotInfo()).
typedef struct {
  bool     present;
  uint16_t seconds;
  uint8_t  mode;
} SlotInfo;
static volatile SlotInfo slotInfo[BREW_SLOT_LAST - BREW_SLOT_FIRST + 1];

static bool readSlotInfo(char slot, SlotInfo *info);
static void refreshSlotInfo();

float brewInternalPressure(void){
  if(!isnan(getCurrentTemp())){
    return getPressureByTemp(getCurrentTemp(), getCurrentPressure());
  }
  return getCurrentPressure() / 100000;
}

static void recordSample(int status){
  float temp = getCurrentTemp();
  BrewSample sample;
  sample.seconds = (uint16_t)((millis() - sessionStartMs) / 1000);
  sample.tempDeci = (isnan(temp) || temp <= -127.0) ? INT16_MIN : (int16_t)lroundf(temp * 10);
  sample.pressureMbar = 0;
  sample.targetDeci = INT16_MIN;
  if(status == STATUS_BREWING){
    float pressure = brewInternalPressure();
    if(!isnan(pressure) && pressure > 0) sample.pressureMbar = (uint16_t)lroundf(pressure * 1000);
    // Manual power has no PID, so only extractions record a target.
    sample.targetDeci = (int16_t)lroundf(getPidTarget() * 10);
  }
  samples[totalSamples % BREW_HISTORY_CAPACITY] = sample;
  totalSamples++;
}

static void historyTask(void *param){
  // Read what the saved slots hold (mounting/formatting SPIFFS if needed)
  // here rather than in the UI or at boot.
  refreshSlotInfo();

  TickType_t lastWake = xTaskGetTickCount();
  int lastStatus = 0;
  while(true){
    int status = getMokaStatus();
    bool recording = (status == STATUS_BREWING || status == STATUS_MANUAL_POWER);

    xSemaphoreTake(historyMutex, portMAX_DELAY);
    if(recording && status != lastStatus){
      // New extraction or manual run: clear the previous history and start
      // a fresh one (the last brew stays available until then).
      sessionId++;
      sessionMode = status;
      totalSamples = 0;
      sessionStartMs = millis();
    }
    sessionActive = recording;
    if(recording) recordSample(status);
    xSemaphoreGive(historyMutex);

    lastStatus = status;
    xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(BREW_HISTORY_PERIOD_MS));
  }
}

void brewHistoryStart(void){
  if(historyMutex != NULL) return;
  historyMutex = xSemaphoreCreateMutex();
  // No I2C: only reads values the sensor tasks already keep.
  xTaskCreatePinnedToCore(historyTask, "history", 3072, NULL, 3, NULL, CORE_UI);
}

// Mounts SPIFFS on first use (formats it if it was never formatted, which can
// take a few seconds once).
static bool ensureSlotsFs(){
  if(!slotsFsReady) slotsFsReady = SPIFFS.begin(true);
  return slotsFsReady;
}

static void slotPath(char slot, char *path, size_t size){
  snprintf(path, size, "/brew%c.bin", slot);
}

BrewSlotResult brewHistorySaveSlot(char slot){
  slot = toupper((unsigned char)slot);
  if(slot < BREW_SLOT_FIRST || slot > BREW_SLOT_LAST) return BREW_SLOT_INVALID;
  if(getMokaStatus() != 0) return BREW_SLOT_NOT_OFF;
  if(historyMutex == NULL) return BREW_SLOT_EMPTY;

  // Copy the recorded samples under the lock, write the file without it.
  xSemaphoreTake(historyMutex, portMAX_DELAY);
  long oldest = totalSamples > BREW_HISTORY_CAPACITY ? totalSamples - BREW_HISTORY_CAPACITY : 0;
  long count = totalSamples - oldest;
  int mode = sessionMode;
  BrewSample *copy = NULL;
  if(count > 0 && mode != 0){
    copy = (BrewSample *)malloc(count * sizeof(BrewSample));
    if(copy){
      for(long k = 0; k < count; k++) copy[k] = samples[(oldest + k) % BREW_HISTORY_CAPACITY];
    }
  }
  xSemaphoreGive(historyMutex);

  if(count <= 0 || mode == 0) return BREW_SLOT_EMPTY;
  if(!copy) return BREW_SLOT_FS_ERROR;
  if(!ensureSlotsFs()){
    free(copy);
    return BREW_SLOT_FS_ERROR;
  }

  char path[16];
  slotPath(slot, path, sizeof(path));
  File file = SPIFFS.open(path, FILE_WRITE);
  bool ok = (bool)file;
  if(ok){
    SlotHeader header = { BREW_SLOT_MAGIC, (uint8_t)mode, 0, (uint16_t)count };
    size_t dataSize = count * sizeof(BrewSample);
    ok = file.write((const uint8_t *)&header, sizeof(header)) == sizeof(header) &&
         file.write((const uint8_t *)copy, dataSize) == dataSize;
    file.close();
  }
  free(copy);
  Serial.printf("Brew history: %s slot %c (%ld samples)\n", ok ? "saved to" : "could not write", slot, count);
  if(ok) refreshSlotInfo();
  return ok ? BREW_SLOT_SAVED : BREW_SLOT_FS_ERROR;
}

BrewSlotResult brewHistoryDeleteSlot(char slot){
  slot = toupper((unsigned char)slot);
  if(slot < BREW_SLOT_FIRST || slot > BREW_SLOT_LAST) return BREW_SLOT_INVALID;
  if(!ensureSlotsFs()) return BREW_SLOT_FS_ERROR;
  char path[16];
  slotPath(slot, path, sizeof(path));
  bool ok = !SPIFFS.exists(path) || SPIFFS.remove(path);
  Serial.printf("Brew history: %s slot %c\n", ok ? "deleted" : "could not delete", slot);
  // The profile page reads the cached info: update it right away.
  refreshSlotInfo();
  return ok ? BREW_SLOT_SAVED : BREW_SLOT_FS_ERROR;
}

// Reads a slot file's header and duration; false when empty or invalid.
// samplesOut (optional) receives the sample count.
static bool readSlotHeader(char slot, SlotInfo *info, uint16_t *samplesOut){
  char path[16];
  slotPath(slot, path, sizeof(path));
  if(!ensureSlotsFs() || !SPIFFS.exists(path)) return false;
  bool found = false;
  File file = SPIFFS.open(path, FILE_READ);
  SlotHeader header;
  if(file && file.read((uint8_t *)&header, sizeof(header)) == sizeof(header) &&
     header.magic == BREW_SLOT_MAGIC && header.count > 0){
    // Duration = time of the last sample.
    BrewSample last;
    file.seek(sizeof(header) + (header.count - 1) * sizeof(BrewSample));
    if(file.read((uint8_t *)&last, sizeof(last)) == sizeof(last)){
      info->present = true;
      info->seconds = last.seconds;
      info->mode = header.mode;
      if(samplesOut) *samplesOut = header.count;
      found = true;
    }
  }
  if(file) file.close();
  return found;
}

static bool readSlotInfo(char slot, SlotInfo *info){
  return readSlotHeader(slot, info, NULL);
}

static void refreshSlotInfo(){
  for(char slot = BREW_SLOT_FIRST; slot <= BREW_SLOT_LAST; slot++){
    SlotInfo info = { false, 0, 0 };
    readSlotInfo(slot, &info);
    int index = slot - BREW_SLOT_FIRST;
    slotInfo[index].seconds = info.seconds;
    slotInfo[index].mode = info.mode;
    slotInfo[index].present = info.present;
  }
}

static bool validSlotIndex(int index){
  return index >= 0 && index <= BREW_SLOT_LAST - BREW_SLOT_FIRST;
}

bool brewSlotPresent(int index){
  return validSlotIndex(index) && slotInfo[index].present;
}

int brewSlotSeconds(int index){
  return validSlotIndex(index) ? slotInfo[index].seconds : 0;
}

int brewSlotMode(int index){
  return validSlotIndex(index) ? slotInfo[index].mode : 0;
}

String brewSlotsJson(){
  String json = "{";
  for(char slot = BREW_SLOT_FIRST; slot <= BREW_SLOT_LAST; slot++){
    if(slot != BREW_SLOT_FIRST) json += ',';
    json += '"';
    json += slot;
    json += "\":";

    SlotInfo info = { false, 0, 0 };
    uint16_t sampleCount = 0;
    if(readSlotHeader(slot, &info, &sampleCount)){
      json += "{\"samples\":";
      json += sampleCount;
      json += ",\"seconds\":";
      json += info.seconds;
      json += ",\"mode\":";
      json += info.mode;
      json += '}';
    }
    else{
      json += "null";
    }
  }
  json += '}';
  return json;
}

String brewHistoryJson(long session, long from){
  if(historyMutex == NULL) return "{\"session\":0,\"mode\":0,\"active\":false,\"next\":0,\"samples\":[]}";
  xSemaphoreTake(historyMutex, portMAX_DELAY);

  long oldest = totalSamples > BREW_HISTORY_CAPACITY ? totalSamples - BREW_HISTORY_CAPACITY : 0;
  // Another session (or a page loading for the first time): send everything.
  if(session != sessionId || from < oldest || from > totalSamples) from = oldest;

  String json;
  json.reserve(64 + (totalSamples - from) * 28);
  json += "{\"session\":";
  json += sessionId;
  json += ",\"mode\":";
  json += sessionMode;
  json += ",\"active\":";
  json += sessionActive ? "true" : "false";
  json += ",\"status\":";
  json += getMokaStatus();
  json += ",\"next\":";
  json += totalSamples;
  json += ",\"samples\":[";
  for(long i = from; i < totalSamples; i++){
    const BrewSample &s = samples[i % BREW_HISTORY_CAPACITY];
    if(i > from) json += ',';
    json += '[';
    json += s.seconds;
    json += ',';
    if(s.tempDeci == INT16_MIN) json += "null";
    else json += String(s.tempDeci / 10.0, 1);
    json += ',';
    if(s.pressureMbar == 0) json += "null";
    else json += String(s.pressureMbar / 1000.0, 3);
    json += ',';
    if(s.targetDeci == INT16_MIN) json += "null";
    else json += String(s.targetDeci / 10.0, 1);
    json += ']';
  }
  json += "]}";

  xSemaphoreGive(historyMutex);
  return json;
}
