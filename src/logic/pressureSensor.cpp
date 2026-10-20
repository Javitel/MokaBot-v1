#include "pressureSensor.hpp"
#include <Adafruit_BMP280.h>
#include <Preferences.h>
#include "logic/moka.hpp"
#include "devices/boardPins.hpp"
#include <Wire.h>

#define BMP280_ADDRESS (0x76)
Adafruit_BMP280 bmp;

Preferences preferences;

float boilingPoint =100.0;
float boilingPointPressure = 1.0;

float readPressure = 0.0;

void initSensor(){
      preferences.begin("my-app", false); 

  boilingPoint = preferences.getFloat("boilingPoint", boilingPoint);
  boilingPointPressure = preferences.getFloat("boilPointPres", boilingPointPressure);
  setTargetBoilingTemp(preferences.getFloat("boilingTemp", getTargetBoilingTemp()));
  setTargetHeatingTemp(preferences.getFloat("heatingTemp", getTargetHeatingTemp()));
  preferences.end();
  // Shared with the ADS1115 (started later in init()). On boards whose
  // display driver already started Wire on these pins, this is a no-op.
  Wire.begin(PIN_SENSOR_SDA, PIN_SENSOR_SCL);
  bmp.begin(BMP280_ADDRESS);
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X16,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_63); /* Standby time. */
                  delay(100);
}

float getCurrentPressure(){
    return readPressure;//bmp.readPressure();
}

float getAmbientTemp(){
    return bmp.readTemperature();
}

void updateCurrentPressure(){
  double aux;
    //while(true){
    try{
    aux = bmp.readPressure();
    } catch (...){bmp.reset();}
    if( aux > 0){
      readPressure = aux;
    }
    
  //}
}
void overrideBoiling(double input){
    preferences.begin("my-app", false);
    boilingPoint = input;
     preferences.putFloat("boilingPoint", boilingPoint);
  boilingPointPressure = readPressure/100000;
  preferences.putFloat("boilPointPres", boilingPointPressure);
  preferences.end();
}
#define CALIBRATION_WINDOW_MS 3000
#define CALIBRATION_SAMPLE_MS 100

static volatile bool calibrating = false;

// Samples the temperature for CALIBRATION_WINDOW_MS and stores the maximum
// (with the pressure read at that moment) as the boiling calibration point.
static void calibrateBoilingTask(void *param){
    float maxTemp = NAN;
    float pressureAtMax = readPressure;
    for(int elapsed = 0; elapsed < CALIBRATION_WINDOW_MS; elapsed += CALIBRATION_SAMPLE_MS){
        float temp = getCurrentTemp();
        // Skip disconnected/invalid readings.
        if(!isnan(temp) && temp > -50.0 && (isnan(maxTemp) || temp > maxTemp)){
            maxTemp = temp;
            pressureAtMax = readPressure;
        }
        vTaskDelay(pdMS_TO_TICKS(CALIBRATION_SAMPLE_MS));
    }

    if(!isnan(maxTemp)){
        preferences.begin("my-app", false);
        boilingPoint = maxTemp;
        preferences.putFloat("boilingPoint", boilingPoint);
        boilingPointPressure = pressureAtMax/100000;
        preferences.putFloat("boilPointPres", boilingPointPressure);
        preferences.end();
    }
    calibrating = false;
    vTaskDelete(NULL);
}

// Non-blocking: runs the sampling window in its own task so the caller (an
// LVGL event) does not freeze the UI. Ignored while a calibration is running.
void calibrateBoiling(){
    if(calibrating) return;
    calibrating = true;
    if(xTaskCreate(calibrateBoilingTask, "calibrate", 3072, NULL, 5, NULL) != pdPASS){
        calibrating = false;
    }
}
float getCalibratedTemp(){
    return boilingPoint;
}
double getCalibratedPressure(){
    return boilingPointPressure;
}

void saveTargetHeatingTemp(float temp){
    preferences.begin("my-app", false);
    preferences.putFloat("heatingTemp", temp);
    preferences.end();
}

void saveTargetBoilingTemp(float temp){
    preferences.begin("my-app", false);
    preferences.putFloat("boilingTemp", temp);
    preferences.end();
}

float getBoilingTemp(float pressure){
    double atm = pressure;// * 0.986923;
      //double aux = log(getCalibratedPressure()/atm);
    double aux = log(atm/getCalibratedPressure());
    aux = (8.3145/40700) * aux;
    aux = (1.0/(getCalibratedTemp()+273.15))-aux;
    aux = (1.0/aux);

    return (float)aux-273.15;
  
}

float getPressureByTemp(float temp, float pressure){
    if(temp < getBoilingTemp(pressure/100000)){
      return getCurrentPressure()/100000;
    }
    else{
      float factor = (40700.0/8.3145)*((1/(getCalibratedTemp()+273))-(1/(temp+273)));
      return (getCalibratedPressure()*exp(factor)/0.986923);
    }
  }