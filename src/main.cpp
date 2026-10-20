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
TaskHandle_t PressureTask = NULL; 

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

void calculateLabels(void *param){
  initCycle = true;
  String lastIp, lastTempText, lastPressureText, lastTargetText, lastExtraText, lastBloomText, lastBrewText;
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
    else{
      targetText = "OFF";
      extraText="";
      bloomText="";
      brewText="";
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
    xSemaphoreGive(lvglMutex);
    initCycle = false;
  }
}

void setup() {
  Serial.begin(115200);
  displayInit();
  beepInit();
  // Beep on button presses (no-op on boards without audio).
  lv_indev_get_next(NULL)->driver->feedback_cb = ui_input_feedback;

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
  xTaskCreatePinnedToCore(process, "process",2048, NULL, 7, &PIDTask,1);     
  xTaskCreatePinnedToCore(webserverService, "webserver",4096, NULL, 3, &WebserviceTask,1);
  xTaskCreatePinnedToCore(updateSensors, "sensors",4096, NULL, 8, &SensorTask,0);
  xTaskCreatePinnedToCore(updateAmbientPressure, "pressure",2048, NULL, 4, &PressureTask,0);
  xTaskCreatePinnedToCore(refreshUI, "UI",4096, NULL, 7, &UITask,0);
  xTaskCreatePinnedToCore(calculateLabels, "labels",4096, NULL, 8, &LabelsTasks,1);
  ui_init();
  initCycle = true;

}

void loop() {
  //Serial.println(bmp.readPressure());
  vTaskDelay(pdMS_TO_TICKS(1000));
}
