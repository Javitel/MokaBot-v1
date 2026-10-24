#include "brewHistory.hpp"
#include "logic/moka.hpp"
#include "logic/pressureSensor.hpp"
#include "taskCores.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BREW_HISTORY_CAPACITY    900   // 15 min at one sample per second
#define BREW_HISTORY_PERIOD_MS   1000
#define STATUS_BREWING           2
#define STATUS_MANUAL_POWER      3

// Compact sample: 6 bytes, 900 samples ~ 5.4 KB.
typedef struct {
  uint16_t seconds;        // since the session started
  int16_t  tempDeci;       // 0.1 C; INT16_MIN = no valid reading
  uint16_t pressureMbar;   // 0 = not recorded (manual power)
} BrewSample;

static BrewSample samples[BREW_HISTORY_CAPACITY];
static long totalSamples = 0;    // samples recorded this session (index of the next one)
static long sessionId = 0;       // incremented at each new session
static int sessionMode = 0;      // 2 or 3 (0 before the first session)
static bool sessionActive = false;
static uint32_t sessionStartMs = 0;
static SemaphoreHandle_t historyMutex = NULL;

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
  if(status == STATUS_BREWING){
    float pressure = brewInternalPressure();
    if(!isnan(pressure) && pressure > 0) sample.pressureMbar = (uint16_t)lroundf(pressure * 1000);
  }
  samples[totalSamples % BREW_HISTORY_CAPACITY] = sample;
  totalSamples++;
}

static void historyTask(void *param){
  TickType_t lastWake = xTaskGetTickCount();
  int lastStatus = 0;
  while(true){
    int status = getMokaStatus();
    bool recording = (status == STATUS_BREWING || status == STATUS_MANUAL_POWER);

    xSemaphoreTake(historyMutex, portMAX_DELAY);
    if(recording && status != lastStatus){
      // New extraction or manual run: start a fresh history.
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

String brewHistoryJson(long session, long from){
  if(historyMutex == NULL) return "{\"session\":0,\"mode\":0,\"active\":false,\"next\":0,\"samples\":[]}";
  xSemaphoreTake(historyMutex, portMAX_DELAY);

  long oldest = totalSamples > BREW_HISTORY_CAPACITY ? totalSamples - BREW_HISTORY_CAPACITY : 0;
  // Another session (or a page loading for the first time): send everything.
  if(session != sessionId || from < oldest || from > totalSamples) from = oldest;

  String json;
  json.reserve(64 + (totalSamples - from) * 22);
  json += "{\"session\":";
  json += sessionId;
  json += ",\"mode\":";
  json += sessionMode;
  json += ",\"active\":";
  json += sessionActive ? "true" : "false";
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
    json += ']';
  }
  json += "]}";

  xSemaphoreGive(historyMutex);
  return json;
}
