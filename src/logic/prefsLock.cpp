#include "prefsLock.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Created during static initialisation, before setup() and any task runs.
static SemaphoreHandle_t prefsMutex = xSemaphoreCreateRecursiveMutex();

void prefsLock(void){
  xSemaphoreTakeRecursive(prefsMutex, portMAX_DELAY);
}

void prefsUnlock(void){
  xSemaphoreGiveRecursive(prefsMutex);
}
