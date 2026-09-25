#include "pressureSensor.hpp"
#include <Adafruit_BMP280.h>
#include <Preferences.h>
#include "logic/moka.hpp"

#define BMP_SDA 21
#define BMP_SCL 22
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
  bmp.begin(0x76);
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
void calibrateBoiling(){
    preferences.begin("my-app", false);
    boilingPoint = getCurrentTemp();
     preferences.putFloat("boilingPoint", boilingPoint);
  boilingPointPressure = readPressure/100000;
  preferences.putFloat("boilPointPres", boilingPointPressure);
  preferences.end();
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
    //Serial.println(getCalibratedPressure());
    //Serial.println(getCalibratedTemp());
    return (float)aux-273.15;
  
}

float getAmbientTemp(){
  return bmp.readTemperature();
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