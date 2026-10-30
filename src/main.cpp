//*********************************************************************************************************/
//  WT32-SC01-PLUS template for platform.io
//  created by Frits Jan / productbakery on 11 oktober 2022
//
//
// When working with the squareline editor from lvgl, set the project in squareline to:
// - Arduino, with TFT_eSPI (which we cannot use, but will replace with LovyanGFX in this main.cpp file)
// - 480 x 320, 16 bit display
// 
// Export the template project AND export the UI Files
// You will get a project directory with two directories inside, 'ui' and 'libraries'
// From the libraries directory, copy the lv_conf.h to this projects /src/ directory (overwrite the old one)
// From the ui directory, copy all files to this projects src/ui/ directory (you can empty the ui directory first if needed)
// The ui.ino file can/should be deleted because this main.cpp files takes over.
//
//*********************************************************************************************************/

#include <Arduino.h>
#include "sdkconfig.h"
#include "logic/moka.hpp"
#include "logic/pressureSensor.hpp"
#include "logic/webserver.hpp"
#include "logic/brewHistory.hpp"
#include "logic/bleLink.hpp"
#include "taskCores.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define USE_DYNAMIC_PARAMETERS      false
bool LOAD_DEFAULT_CONFIG_DATA = true;

#include <lvgl.h>
#include "ui/ui.h" // this is the ui generated with lvgl / squareline editor
// Display/touch driver, selected with a BOARD_* build flag in platformio.ini.
#if defined(BOARD_WAVESHARE_S3_TOUCH_LCD_35B)
#include "devices/waveshare-s3-touch-lcd-3.5b.hpp"
#else
#include "devices/wt32-sc01.hpp"
#endif
#include "devices/beep.hpp"

//************************************************************************************
//  SETUP AND LOOP
//*********************************************************

int frame = 0;
float currentPressure;
float currentTemp;
float aux;
bool initCycle = true;
String text;

TaskHandle_t PIDTask = NULL; 
TaskHandle_t UITask = NULL;  
TaskHandle_t WebserviceTask = NULL;  
TaskHandle_t SensorTask = NULL;  
TaskHandle_t LabelsTasks = NULL;  
TaskHandle_t AmbientDataTask = NULL; 

String prepareLabel(float value, int precision, String label){
  return String(value, precision)+label;
}

String htmlTemp() {
    return String(currentTemp);
}

// LVGL is not thread safe: the UI task and the labels task share this lock.
static SemaphoreHandle_t lvglMutex;

void refreshUI(void *param){
  /*Initialize the input device driver*/
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(20);
  BaseType_t xWasDelayed;
  xLastWakeTime = xTaskGetTickCount ();
  while(true){
    xSemaphoreTake(lvglMutex, portMAX_DELAY);
    lv_timer_handler();
    xSemaphoreGive(lvglMutex);
    xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
  }
}

// Calls setter only when the text differs from the last one applied, so
// unchanged labels are not invalidated (and redrawn) on every refresh.
static void updateLabel(String &last,const String &text, void (*setter)(const char *)){
  if( !initCycle && text == last){ return;}
  last = text;
  setter(text.c_str());
}

String formatValue(float value, const char *prefix, const char *suffix){
  return String(prefix) + String(value, ui_show_decimals() ? 1 : 0) + suffix;
}

// Formats a Celsius value in the unit selected on the screen.
String formatTemp(float celsius, const char *prefix){
  if(ui_use_fahrenheit()) return formatValue(celsius * 9.0 / 5.0 + 32.0, prefix, "°F");
  return formatValue(celsius, prefix, "°C");
}

#define BLE_FLASH_MS        1500
#define BLE_FLASH_PERIOD_MS 250

void calculateLabels(void *param){
  initCycle = true;
  bool lastBleConnected = false;
  uint32_t bleConnectedAt = 0;
  bool lastBleFlashing = false;
  String lastIp, lastTempText, lastPressureText, lastTargetText, lastExtraText, lastBloomText, lastBrewText, lastExtractText, lastProfileStatusText;
  while(true){
    vTaskDelay( pdMS_TO_TICKS( 50 ));
    currentPressure = getCurrentPressure();
    currentTemp = getCurrentTemp();
    int status = getMokaStatus();

    // Build every label text first, without touching LVGL.
    String ip = getIp();
    String tempText = (currentTemp <= -50.0 || isnan(currentTemp)) ? String("DISC.") : formatTemp(currentTemp, "");
    // Pressure moves in thousandths of a bar, so it needs more precision than
    // temperatures; 1 decimal would freeze the label at "1.0bar".
    String pressureText = prepareLabel(currentPressure/100000, 3, "bar");
    String targetText, extraText, bloomText, brewText;
    String extractText = "EXTRACT";
    String profileStatusText;

    if(status == 1){
      bloomText="";
      brewText="";
      targetText = formatTemp(getTargetHeatingTemp(), "");
      extraText = formatTemp(getBoilingTemp(currentPressure/100000), "Boils at: ");
    }
    else if(status == 2){
      targetText = formatTemp(getTargetBoilingTemp(), "");
      extraText = "Pressure: " + String(getPressureByTemp(getTargetBoilingTemp(),getCalibratedPressure())) + "bar";
      bloomText = (getBloomTime() == -1) ? String("OFF") : formatValue(getBloomTime(), "Bloom: ", "s");
      brewText = formatValue(getBrewTime(), "Brew: ", "s");
    }
    else if(status == 3){
      targetText = String(getManualPower()) + "%";
      extraText = "ON";
      bloomText="";
      brewText="";
    }
    else{
      targetText = "OFF";
      extraText="";
      bloomText="";
      brewText="";
    }
    // Power page (swiped to from the main screen): the target label shows the
    // power percentage instead of the target temperature.
    if(ui_power_page()){
      targetText = String(ui_get_power_percent()) + "%";
      // Info line shows whether manual power is running.
      extraText = (status == 3) ? "ON" : "OFF";
    }
    // Profile page: the selected saved brew instead of a target.
    if(ui_profile_page()){
      int profile = ui_get_selected_profile();
      if(profile < 0){
        targetText = "Empty";
        extraText = "";
      }
      else{
        targetText = String((char)('A' + profile));
        int seconds = brewSlotSeconds(profile);
        int elapsed = brewProfileElapsed();
        char info[32];
        if(elapsed >= 0){
          // Replaying: progress of the saved brew.
          if(elapsed > seconds) elapsed = seconds;
          snprintf(info, sizeof(info), "%02d:%02d / %02d:%02d", elapsed / 60, elapsed % 60, seconds / 60, seconds % 60);
        }
        else{
          snprintf(info, sizeof(info), "%02d:%02d %s", seconds / 60, seconds % 60,
                   brewSlotMode(profile) == 3 ? "manual power" : "extraction");
        }
        extraText = info;
        // Under the target letter, where PREHEAT is on the extract page:
        // target temperature and mode. While OFF, the preheat temperature
        // the profile will start from.
        if(status == 1) profileStatusText = formatTemp(getTargetHeatingTemp(), "") + "\npreheating";
        else if(status == 2) profileStatusText = formatTemp(getPidTarget(), "") + "\nextracting";
        else if(status == 3) profileStatusText = String(getManualPower()) + "%\nmanual power";
        else{
          float preheat = brewSlotPreheat(profile);
          profileStatusText = (isnan(preheat) ? String("--") : formatTemp(preheat, "")) + "\noff";
        }
        // Ended early: the boiler ran low on water.
        if(status == 0 && profileStoppedByOvershoot()) extraText = "Ended: overshoot";
        if(brewProfileNeedsPreheat(profile)) extractText = "PREHEAT";
      }
    }

    // A boiling calibration message (running / result / refused) takes the
    // info line for a few seconds.
    float calibrationTemp;
    const char *calibration = calibrationMessage(&calibrationTemp);
    if(calibration[0] != '\0'){
      extraText = isnan(calibrationTemp) ? String(calibration) : formatTemp(calibrationTemp, calibration);
    }

    // Apply only what changed.
    xSemaphoreTake(lvglMutex, portMAX_DELAY);
    updateLabel(lastIp, ip, setCurrentIP);
    updateLabel(lastTempText, tempText, setCurrentTempLabel);
    updateLabel(lastPressureText, pressureText, setCurrentPressure);
    updateLabel(lastTargetText, targetText, setTargetTempLabel);
    updateLabel(lastExtraText, extraText, setExtraInfo);
    updateLabel(lastBloomText, bloomText, setProWeigth);
    updateLabel(lastBrewText, brewText, setProTemp);
    updateLabel(lastExtractText, extractText, setExtractButtonLabel);
    updateLabel(lastProfileStatusText, profileStatusText, setProfileStatusLabel);
    // Blue square while a phone is connected over Bluetooth; it flashes for
    // BLE_FLASH_MS after connecting, to tell which MokaBot the phone joined.
    bool ble = bleConnected();
    if(ble && !lastBleConnected) bleConnectedAt = millis();
    bool flashing = ble && millis() - bleConnectedAt < BLE_FLASH_MS;
    if(initCycle || ble != lastBleConnected || flashing || lastBleFlashing){
      ui_set_ble_indicator(ble && (!flashing || ((millis() - bleConnectedAt) / BLE_FLASH_PERIOD_MS) % 2 == 0));
    }
    lastBleConnected = ble;
    lastBleFlashing = flashing;
    xSemaphoreGive(lvglMutex);
    initCycle = false;
  }
}

void setup() {
  Serial.begin(115200);
  displayInit();
  beepInit();

  // Boot logo. LVGL only draws inside lv_timer_handler()/lv_refr_now(), so it
  // is rendered explicitly here, before the UI task exists.
  ui_initBoot();
  lv_refr_now(NULL);
  uint32_t bootStart = millis();

  initSensor();
  init();

  // Keep the logo up for at least a second, then switch to the main screen.
  while(millis() - bootStart < 1500){
    lv_timer_handler();
    delay(10);
  }
  currentPressure = getCurrentPressure();
  lvglMutex = xSemaphoreCreateMutex();
  // I2C users (PID reads the BMP280, sensors, pressure) on CORE_I2C, with the
  // touch task; LVGL drawing and label updates alone on CORE_UI.
  // Records the brew curve for the brew page (before the web server starts).
  brewHistoryStart();
  xTaskCreatePinnedToCore(process, "process",2048, NULL, 5, &PIDTask, CORE_UI);
  xTaskCreatePinnedToCore(updateSensors, "sensors",4096, NULL, 5, &SensorTask, CORE_I2C);
  // 4096: the BMP read path plus ESP-IDF's I2C error logging (printf)
  // overflowed 2048 ("Stack canary watchpoint triggered (pressure)").
  xTaskCreatePinnedToCore(getAmbientData, "ambientData",4096, NULL, 4, &AmbientDataTask, CORE_I2C);
  xTaskCreatePinnedToCore(refreshUI, "UI",4096, NULL, 7, &UITask, CORE_UI);
  xTaskCreatePinnedToCore(calculateLabels, "labels",4096, NULL, 6, &LabelsTasks, CORE_UI);
  // No I2C; next to the WiFi stack (core 0) at low priority.
  xTaskCreatePinnedToCore(webserverService, "webserver",4096, NULL, 3, &WebserviceTask, CORE_I2C);
  ui_init();
  // Bluetooth link for the Android app (no-op on boards built without MOKA_BLE).
  bleStart();
  initCycle = true;

}

void loop() {
  //Serial.println(bmp.readPressure());
  vTaskDelay(pdMS_TO_TICKS(1000));
}
