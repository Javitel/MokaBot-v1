#include "pro.hpp"
#include "HX711.h"
#include <Preferences.h>
#include <max6675.h>
/*!
 * @fn DFRobot_HX711
 * @brief Constructor 
 * @param pin_din  Analog data pins
 * @param pin_slk  Analog data pins
 */
uint8_t proConnected = 12;

int scaleData =34;
int scaleClock =4;

int thermoDO = 27;
int thermoCS = 32;
int thermoCLK = 21;

double baseTemp=0.0;
double grams = 0.0;

HX711 scale;
MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);
Preferences preferencesScale;

void proRoutine(void *param){
pinMode(proConnected, INPUT_PULLUP);
  while(digitalRead(proConnected)==HIGH){
    Serial.print("Disconnected pro" );
     vTaskDelay( pdMS_TO_TICKS( 500 ));
  }
  preferencesScale.begin("my-app", false); 
  float offset = preferencesScale.getFloat("offset", 0.0);
  float weigthScale = preferencesScale.getFloat("scale", 0.0);
  scale.begin(scaleData, scaleClock);
  vTaskDelay( pdMS_TO_TICKS( 500 ));
  scale.set_offset(offset);
  scale.set_scale(weigthScale);
  preferencesScale.end();
  scale.set_rate_80SPS();
  while(true){
    while(digitalRead(proConnected)==HIGH){
        Serial.print("Disconnected pro" );
        vTaskDelay( pdMS_TO_TICKS( 500 ));
    }
    grams = scale.get_units(10);
    baseTemp = thermocouple.readCelsius();
    Serial.println("weight " + (String)grams);
    Serial.println("temp " + (String)baseTemp);
    vTaskDelay( pdMS_TO_TICKS( 20 ));
  }
}

int isBaseConnected(){
    if(digitalRead(proConnected)==LOW){
        return 1;
    }else{
        return 0;
    }
}

double getBaseTemp(){
    return baseTemp;
}

double getWeight(){
    return grams;
}

void tare(int longPress){
    if(isBaseConnected() ==1){
        if(longPress == 0){
            scale.tare();
        }
        else{
            for (uint8_t t=0; t<10; t++) {
                scale.tare();
                vTaskDelay( pdMS_TO_TICKS( 50 ));
            }
        }
    }
}

void calibrateCellSensor(double weigth){
    if(isBaseConnected() ==1){
        Serial.print("UNITS: ");
  //vTaskDelay( pdMS_TO_TICKS( 500 ));
        scale.calibrate_scale(weigth, 25);
        
        preferencesScale.begin("my-app", false);
        preferencesScale.putFloat("offset", scale.get_offset());
        preferencesScale.putFloat("scale", scale.get_scale());
        preferencesScale.end();
    }
  
}