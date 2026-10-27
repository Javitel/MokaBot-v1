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
static uint8_t sessionPower = 0;                 // manual power percentage
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
  uint8_t  power;      // manual power percentage (0 = not recorded, older files)
  uint16_t count;      // samples that follow
} SlotHeader;

static bool slotsFsReady = false;

// What each slot holds, cached for the UI (see refreshSlotInfo()).
typedef struct {
  bool     present;
  uint16_t seconds;
  uint8_t  mode;
  uint8_t  power;
  int16_t  preheatDeci;   // temperature of the first sample, 0.1 C; INT16_MIN = none
} SlotInfo;
static volatile SlotInfo slotInfo[BREW_SLOT_LAST - BREW_SLOT_FIRST + 1];

static bool readSlotInfo(char slot, SlotInfo *info);
static void refreshSlotInfo();

// ---- Profile playback ----
// A saved brew replayed from the profile page: an extraction follows the
// saved PID target second by second, manual power runs at the saved
// percentage; both stop at the saved duration (or when the status changes).
static bool playing = false;
static int playMode = 0;
static int playSeconds = 0;          // saved duration
static int16_t *playTargets = NULL;  // target per second (0.1 C), extractions only
static uint32_t playStartMs = 0;

static void stopPlayback(){
  playing = false;
  free(playTargets);
  playTargets = NULL;
}

// Called every second by the history task, with the lock held.
static void updatePlayback(int status){
  if(!playing) return;
  // Switched off, over-temperature, cut-off, another mode...
  if(status != playMode){
    stopPlayback();
    return;
  }
  int elapsed = (millis() - playStartMs) / 1000;
  if(elapsed > playSeconds){
    stopPlayback();
    setMokaStatus(0);
    return;
  }
  if(playMode == STATUS_BREWING) setProfileTarget(playTargets[elapsed] / 10.0);
}

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
    // Status read with the lock held: brewProfileStart() changes it under
    // the same lock.
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    int status = getMokaStatus();
    bool recording = (status == STATUS_BREWING || status == STATUS_MANUAL_POWER);
    if(recording && status != lastStatus){
      // New extraction or manual run: clear the previous history and start
      // a fresh one (the last brew stays available until then).
      sessionId++;
      sessionMode = status;
      totalSamples = 0;
      sessionStartMs = millis();
      // Saved with the brew, for replaying it as a profile.
      sessionPower = (uint8_t)getManualPower();
    }
    sessionActive = recording;
    if(recording) recordSample(status);
    updatePlayback(status);
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
  uint8_t power = sessionPower;
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
    SlotHeader header = { BREW_SLOT_MAGIC, (uint8_t)mode, power, (uint16_t)count };
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

// Reads the header of an open slot file. Leaves the file at the first sample.
static bool readHeader(File &file, SlotHeader *header){
  return file.read((uint8_t *)header, sizeof(*header)) == sizeof(*header) &&
         header->magic == BREW_SLOT_MAGIC && header->count > 0;
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
  if(file && readHeader(file, &header)){
    // Preheat temperature = temperature when the brew started (first
    // sample); duration = time of the last sample.
    BrewSample first, last;
    bool ok = file.read((uint8_t *)&first, sizeof(first)) == sizeof(first);
    file.seek(sizeof(header) + (header.count - 1) * sizeof(BrewSample));
    if(ok && file.read((uint8_t *)&last, sizeof(last)) == sizeof(last)){
      info->present = true;
      info->seconds = last.seconds;
      info->mode = header.mode;
      info->power = header.power;
      info->preheatDeci = first.tempDeci;
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
    SlotInfo info = { false, 0, 0, 0, INT16_MIN };
    readSlotInfo(slot, &info);
    int index = slot - BREW_SLOT_FIRST;
    slotInfo[index].seconds = info.seconds;
    slotInfo[index].mode = info.mode;
    slotInfo[index].power = info.power;
    slotInfo[index].preheatDeci = info.preheatDeci;
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

float brewSlotPreheat(int index){
  if(!brewSlotPresent(index) || slotInfo[index].preheatDeci == INT16_MIN) return NAN;
  return slotInfo[index].preheatDeci / 10.0;
}

bool brewProfileNeedsPreheat(int index){
  int status = getMokaStatus();
  if(status != 0 && status != 1) return false;
  float preheat = brewSlotPreheat(index);
  if(isnan(preheat)) return false;
  // Preheat cannot go above its limit: EXTRACT once that is reached.
  if(preheat > getHeatingLimit()) preheat = getHeatingLimit();
  float temp = getCurrentTemp();
  return isnan(temp) || temp < preheat;
}

// Reads the saved PID target of every second of an extraction slot into a
// new array of seconds + 1 values (gaps filled with the previous target).
static int16_t *loadSlotTargets(char slot, int seconds){
  char path[16];
  slotPath(slot, path, sizeof(path));
  if(!ensureSlotsFs()) return NULL;
  File file = SPIFFS.open(path, FILE_READ);
  if(!file) return NULL;
  int16_t *targets = NULL;
  SlotHeader header;
  if(readHeader(file, &header)){
    targets = (int16_t *)malloc((seconds + 1) * sizeof(int16_t));
    if(targets){
      for(int s = 0; s <= seconds; s++) targets[s] = INT16_MIN;
      BrewSample sample;
      for(uint16_t k = 0; k < header.count; k++){
        if(file.read((uint8_t *)&sample, sizeof(sample)) != sizeof(sample)) break;
        if(sample.seconds <= seconds) targets[sample.seconds] = sample.targetDeci;
      }
      int16_t last = INT16_MIN;
      for(int s = 0; s <= seconds; s++){
        if(targets[s] == INT16_MIN) targets[s] = last;
        else last = targets[s];
      }
      // No target at all (or none before the first gap): nothing to follow.
      if(targets[0] == INT16_MIN){
        free(targets);
        targets = NULL;
      }
    }
  }
  file.close();
  return targets;
}

bool brewProfileStart(int index){
  if(!brewSlotPresent(index) || historyMutex == NULL) return false;
  int mode = slotInfo[index].mode;
  int seconds = slotInfo[index].seconds;
  int16_t *targets = NULL;
  if(mode == STATUS_BREWING){
    targets = loadSlotTargets(BREW_SLOT_FIRST + index, seconds);
    if(!targets) return false;
  }
  else if(mode != STATUS_MANUAL_POWER){
    return false;
  }

  xSemaphoreTake(historyMutex, portMAX_DELAY);
  stopPlayback();
  playMode = mode;
  playSeconds = seconds;
  playTargets = targets;
  playStartMs = millis();
  playing = true;
  if(mode == STATUS_BREWING){
    profileBrewOn(targets[0] / 10.0);
  }
  else{
    // Slots saved before the percentage was recorded: the last one used.
    int power = slotInfo[index].power;
    manualPowerOn(power > 0 ? power : getManualPower());
  }
  // Refused (no valid temperature reading).
  bool started = getMokaStatus() == mode;
  if(!started) stopPlayback();
  xSemaphoreGive(historyMutex);
  Serial.printf("Profile %c: %s\n", BREW_SLOT_FIRST + index, started ? "started" : "could not start");
  return started;
}

int brewProfileElapsed(void){
  if(historyMutex == NULL) return -1;
  xSemaphoreTake(historyMutex, portMAX_DELAY);
  int elapsed = playing ? (int)((millis() - playStartMs) / 1000) : -1;
  xSemaphoreGive(historyMutex);
  return elapsed;
}

String brewSlotsJson(){
  String json = "{";
  for(char slot = BREW_SLOT_FIRST; slot <= BREW_SLOT_LAST; slot++){
    if(slot != BREW_SLOT_FIRST) json += ',';
    json += '"';
    json += slot;
    json += "\":";

    SlotInfo info = { false, 0, 0, 0, INT16_MIN };
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
