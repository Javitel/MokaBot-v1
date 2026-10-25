#include <Arduino.h>
#include <Wire.h> 
//#include <Adafruit_ADS1X15.h>
#include "ADS1X15.h"
#include <AutoPID.h>
#include "moka.hpp"
#include <Preferences.h>
#include "logic/prefsLock.hpp"
#include <time.h>
#include "logic/pressureSensor.hpp"
#include "devices/boardPins.hpp"
#include "taskCores.hpp"

const int RelayActivationValue = HIGH;
//const int pinDataDQ = 33;
const int pinHeater = PIN_HEATER;
//const int pinNTC = 27;

boolean running = false;
boolean changePending = false;

//Adafruit_ADS1115 ads;
ADS1115 ads(0x48);

double gain=1.0;

double heat = 92.0;
double boil = 106.0;
double target =0.0;
double temperature;
double bloomDefaultTargetTime = 0;
double bloomCurrentTargetTime = 0;
double bloomOffset = 0;
// Manual power mode switches itself off this many degrees above the boiling
// point (may be negative). Saved in Preferences, set from the settings page.
int manualCutoffAboveBoiling = 3;
boolean bloomfinished = false;
boolean brewstart = false;
double bloomTime = 0;
double brewTime = 0;
double startTime = 0;
double elapsedTime = 0;

double refVoltage = 0;
double resistorFixed = 2000;
//double ntcRefResistor=50000;
//double ntcBeta=3950;
// Steinhart-Hart coefficients of the NTC.
double shA=0.9032071364/1000;
double shB=2.086434938/10000;
double shC=1.597510723/10000000;
double ntcValue = 0;

double current = 0.0;
int mokaStatus = 0;
bool connected=false;

double defaultp = 40.2;//0.03 0.003;
double defaulti = 0.00075;//0.00045;//0.0004;
double defaultd = 0.085;//0.000002;

double p, i, d;  

double timeWindow =250;
double pidValue;
AutoPID myPID(&temperature, &target, &pidValue, 0, timeWindow, defaultp, defaulti, defaultd);

Preferences MokaPreferences;

// Handle of the running brewTimer task, NULL when it is not running.
static TaskHandle_t brewTimerTask = NULL;

int getrelayStatus(int status){
  if (status == LOW){
    if(RelayActivationValue == LOW)
      return HIGH;
    else
      return LOW;
  }
  else{
    if(RelayActivationValue == LOW)
      return LOW;
    else
      return HIGH;
  }
}

  double averageADSRead(uint16_t id, int samples, double margin, double ref){
    // Signed: readings near 0 V can be slightly negative, which wrapped the
    // old unsigned sum (e.g. "result average: 429496728").
    int32_t average = 0;
    int16_t aux = 0;
    ads.setMode(0);
    ads.requestADC(id);
    //ads.startADCReading(id, /*continuous=*/true);
    vTaskDelay( pdMS_TO_TICKS( 30));//delay(10);
    boolean retry =false;
    int valid = 0;
    for (uint8_t i = 0; i < samples; i++) {
      // Skip samples whose I2C read failed (the library reports it through
      // getError(); the catch is only a last resort).
      bool ok = false;
      try{
        aux = ads.getValue();
        ok = ads.getError() == ADS1X15_OK;
      } catch (...){}
      if(ok){
        if((aux > ref + margin || aux < ref - margin) && ref > 0 && retry == false){
          i--;
          retry = true;
        }
        else{
          retry = false;
          average += aux;
          valid++;
        }
      }

      vTaskDelay( pdMS_TO_TICKS( 15));//delay(10);
    }
    // No valid sample: NAN, the caller skips this reading.
    if(valid == 0) return NAN;
    Serial.print("result  average: ");
    Serial.println((double)average/valid);
    return (double)average/valid;
  }

void updateSensors(void *param){
  
   //ads.startADCReading(0, true);
     TickType_t xLastWakeTime = xTaskGetTickCount();
const TickType_t xFrequency = pdMS_TO_TICKS(150);
BaseType_t xWasDelayed;
  while(true){
      try{
      int16_t raw = ads.readADC(3);
      if(ads.getError() == ADS1X15_OK){
        refVoltage = ads.toVoltage(1)*raw;
      }
      updateTempNTC();
      } catch (...){}
    //vTaskDelay(pdMS_TO_TICKS(50));
    xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
  }
}

void getAmbientData(void *param){

    // Initialise the xLastWakeTime variable with the current time.
TickType_t xLastWakeTime = xTaskGetTickCount();
const TickType_t xFrequency = pdMS_TO_TICKS(2000);
BaseType_t xWasDelayed;
  while(true){
        updateCurrentPressure();
        // Ambient temperature too: the PID task reads it from a variable.
        updateAmbientTemp();
        xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
  }
}

void recoverPid(){
  p = MokaPreferences.getFloat("savedP", defaultp);
  i = MokaPreferences.getFloat("savedI", defaulti);
  d = MokaPreferences.getFloat("savedD", defaultd);
}
void recoverResistors(){
  resistorFixed = MokaPreferences.getFloat("resistorFixed", resistorFixed);
  //ntcRefResistor = MokaPreferences.getFloat("ntcRefResistor", ntcRefResistor);
  //ntcBeta = MokaPreferences.getFloat("ntcBeta", ntcBeta);
  shA = MokaPreferences.getDouble("shA", shA);
  shB = MokaPreferences.getDouble("shB", shB);
  shC = MokaPreferences.getDouble("shC", shC);
}

void recoverBrewSettings(){
  bloomDefaultTargetTime = MokaPreferences.getFloat("bloomTime", bloomDefaultTargetTime);
  bloomCurrentTargetTime = MokaPreferences.getFloat("bloomTime", bloomCurrentTargetTime);
  bloomOffset = MokaPreferences.getFloat("bloomOffset", bloomOffset);
  manualCutoffAboveBoiling = MokaPreferences.getInt("cutoffAbove", manualCutoffAboveBoiling);

}

void savePid(double newP, double newI, double newD){
  MokaPreferences.putFloat("savedP", newP);
  MokaPreferences.putFloat("savedI", newI);
  MokaPreferences.putFloat("savedD", newD);
}
void saveResistorValues(double newR, double newA, double newB, double newC){
  MokaPreferences.putFloat("resistorFixed", newR);
  //MokaPreferences.putFloat("ntcRefResistor", newN);
  //MokaPreferences.putFloat("ntcBeta", newB);
  // Doubles: the coefficients are tiny and need the extra precision.
  MokaPreferences.putDouble("shA", newA);
  MokaPreferences.putDouble("shB", newB);
  MokaPreferences.putDouble("shC", newC);
}

void init() {
//Wire.setTimeOut(100);
myPID.setBangBang(12,1);
pinMode(pinHeater, OUTPUT);
digitalWrite(pinHeater,getrelayStatus(LOW));
//ads.setGain(GAIN_TWOTHIRDS);
//ads.setDataRate(RATE_ADS1115_860SPS);
ads.setGain(0); 
ads.setDataRate(7); 
ads.begin();
prefsLock(); MokaPreferences.begin("my-app", false);
recoverPid();
recoverResistors();
recoverBrewSettings();
MokaPreferences.end(); prefsUnlock(); 
myPID.setGains(p,i,d);
}


// Boiling point (C) at the current ambient pressure; the calibrated boiling
// temperature when no valid pressure reading is available yet.
static float currentBoilingPoint(){
  float pressure = getCurrentPressure();
  if(pressure > 0) return getBoilingTemp(pressure / 100000);
  return getCalibratedTemp();
}

// Defined with the other manual power functions below.
static void manualPowerUpdate();

void process(void *param){
 // TickType_t startTime = xTaskGetTickCount();
  TickType_t xLastWakeTime = xTaskGetTickCount();
const TickType_t xFrequency = pdMS_TO_TICKS(250);
BaseType_t xWasDelayed;
  while(true){
//vTaskDelay((150) / portTICK_PERIOD_MS);
    //updateCurrentPressure();
     if(current <= -127.0 || mokaStatus == 0){
      connected=false;
      digitalWrite(pinHeater,getrelayStatus(LOW));
      // Manual power has no temperature control: losing the sensor stops it
      // for good instead of resuming when the reading comes back.
      if(mokaStatus == 3) setMokaStatus(0);
    }
    else if(mokaStatus == 3){
      // Manual power: fixed duty cycle, no PID, until the boiling point
      // (at the current pressure) + manualCutoffAboveBoiling is reached.
      connected=true;
      if(current >= currentBoilingPoint() + manualCutoffAboveBoiling){
        digitalWrite(pinHeater,getrelayStatus(LOW));
        setMokaStatus(0);
      }
      else{
        manualPowerUpdate();
      }
    }
    else{  
      connected=true;
        if(!running){
    pidUpdate();
    if(getMokaStatus() == 2 && brewstart == false && (current) >= getCalibratedTemp()+ bloomOffset){
      brewstart = true;
      startTime = millis() - elapsedTime;
      //startTime = xTaskGetTickCount();
    }
    if(changePending){
      changePending = false;
      if(getMokaStatus() == 1){
          target = heat;
      }
      if(getMokaStatus() == 2){
        if(bloomfinished == false && bloomCurrentTargetTime > 0){
          target = getCalibratedTemp() + bloomOffset;
        }
        else{
          target = boil;
        }
        
      }
      estimateIntegral();
    }
  }

  }
    // Start the brew/bloom timer task once the brew begins; it deletes itself
    // as soon as the status leaves 2.
    if(getMokaStatus() == 2 && brewstart == true && brewTimerTask == NULL){
      // No I2C: runs on the UI core, away from the bus traffic.
      xTaskCreatePinnedToCore(brewTimer, "brewTimer", 2048, NULL, 4, &brewTimerTask, CORE_UI);
    }
xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
    //vTaskDelay( pdMS_TO_TICKS( 30 ));
}

}

// Updates brew and bloom times every 100 ms while extracting. Created by
// process() when the brew starts; runs in its own task because process()
// blocks for a whole PID time window in pidUpdate(). Ends itself as soon as
// the moka status is no longer 2.
void brewTimer(void *param){
  double aux;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(100);
  while(getMokaStatus() == 2){
    aux = millis() - startTime + elapsedTime;

    if(bloomTime >= bloomCurrentTargetTime || bloomfinished == true){
      bloomfinished = true;
      target = boil;
    }
    else{bloomTime = aux/1000;}
    brewTime = aux/1000;
    xTaskDelayUntil( &xLastWakeTime, xFrequency );
  }
  brewTimerTask = NULL;
  vTaskDelete(NULL);
}

void setBloomTime(double overrideTime){
    prefsLock(); MokaPreferences.begin("my-app", false);
    bloomCurrentTargetTime = overrideTime;
    bloomDefaultTargetTime= overrideTime;
    MokaPreferences.putFloat("bloomTime", bloomDefaultTargetTime);
    MokaPreferences.end(); prefsUnlock();
}
void setManualBloomTime(double overrideTime){
    bloomCurrentTargetTime = overrideTime;
}
void skipBloom(){
  if(brewstart == true){
    bloomCurrentTargetTime = bloomDefaultTargetTime;
    bloomfinished = true;
  }
    
}
void setManualCutoff(int degreesAboveBoiling){
    prefsLock(); MokaPreferences.begin("my-app", false);
    manualCutoffAboveBoiling = degreesAboveBoiling;
    MokaPreferences.putInt("cutoffAbove", manualCutoffAboveBoiling);
    MokaPreferences.end(); prefsUnlock();
}

int getManualCutoff(){
  return manualCutoffAboveBoiling;
}

void setBloomOffset(double overrideTemp){
    prefsLock(); MokaPreferences.begin("my-app", false);
    bloomOffset = overrideTemp;
    MokaPreferences.putFloat("bloomOffset", bloomOffset);
    MokaPreferences.end(); prefsUnlock();
}

double getBloomTargetTime(){

  return bloomCurrentTargetTime;
}

double getBloomTime(){
  if(bloomCurrentTargetTime > 0 ){
    return bloomTime;
  }
  else return -1;
}

double getBloomOffset(){
  return bloomOffset;
}

double getBrewTime(){
  return brewTime;
}

// Manual power mode (status 3): no PID, the relay is on for
// manualPowerPercent of every time window.
static volatile int manualPowerPercent = 100;

void setManualPower(int percent){
  if(percent < 0) percent = 0;
  if(percent > 100) percent = 100;
  manualPowerPercent = percent;
}

int getManualPower(){
  return manualPowerPercent;
}

// True when the temperature reading is valid (not disconnected / -127).
static bool temperatureValid(){
  return !isnan(current) && current > -127.0;
}

// Refuses to start without a valid temperature reading.
void manualPowerOn(int percent){
  if(!temperatureValid()) return;
  setManualPower(percent);
  setMokaStatus(3);
}

// One time window in manual mode: heater on for manualPowerPercent of it.
static void manualPowerUpdate(){
  running = true;
  double onTime = timeWindow * manualPowerPercent / 100.0;
  if(onTime > 0){
    digitalWrite(pinHeater,getrelayStatus(HIGH));
    vTaskDelay( pdMS_TO_TICKS( onTime ));
  }
  digitalWrite(pinHeater,getrelayStatus(LOW));
  if(timeWindow - onTime > 0){
    vTaskDelay( pdMS_TO_TICKS( timeWindow - onTime ));
  }
  running = false;
}

 void pidUpdate(){
   running = true;
   temperature=current;
   myPID.run();
    //if(pidValue>0){
     digitalWrite(pinHeater,getrelayStatus(HIGH));
     vTaskDelay( pdMS_TO_TICKS( pidValue * gain ));//}
     //if(pidValue<timeWindow){
      digitalWrite(pinHeater,getrelayStatus(LOW));
      vTaskDelay( pdMS_TO_TICKS( timeWindow-(pidValue * gain)));//}
   running = false;
 }

// Consecutive failed NTC readings after which the sensor is treated as
// disconnected (-127), which turns the heater off in process().
#define NTC_MAX_FAILED_READS 5
static int failedNtcReads = 0;

void updateTempNTC(){
  // Read both channels first; if either failed (NAN), keep the previous
  // temperature instead of computing one from bad data.
  double refRead = averageADSRead(3,5, 25, refVoltage);//ads.readADC_Differential_1_3();//
  double ntcRead = averageADSRead(0,10, 100, ntcValue);//ads.readADC_Differential_0_3();//
  if(isnan(refRead) || isnan(ntcRead)){
    if(++failedNtcReads >= NTC_MAX_FAILED_READS){
      current = -127.0f;
    }
    return;
  }
  failedNtcReads = 0;
  refVoltage = refRead;
  //refVoltage= refVoltage * 0.1875/1000;  f
  refVoltage = ads.toVoltage(1)*refVoltage;
  double temp;

double resistance;
 double lnR;
 double kelvin;
 double vm = 0.0;
 double aux = 0;
vm = ads.toVoltage(1)*ntcRead;
  // NTC divider open or shorted (0 V or at the reference): no temperature can
  // be computed, report disconnected right away instead of NAN. ntcValue is
  // reset so the outlier filter does not compare against this reading.
  if(vm <= 0 || vm >= refVoltage){
    ntcValue = 0;
    current = -127.0f;
    return;
  }
  ntcValue = ntcRead;
resistance = (refVoltage - vm)*(resistorFixed/vm);
aux = shA + (shB * log(resistance)) + (shC * pow(log(resistance), 3));
kelvin =1.0/aux;
if(abs((((kelvin- 273.15) + temp) /2) - temp) < 1){
  temp = ((kelvin- 273.15) * 0.75 )+ temp * 0.25;
}
else{
  temp = kelvin- 273.15;
}

  Serial.println(temp);
  // Serial.println("end");
  if(temp>0){ 
      current=(temp);
    //}
  }
  else{current = -127.0f; }

}

float getCurrentTemp(){
    return current;
  }

int getMokaStatus(){
  return mokaStatus;
}
//0 OFF, 1 HEAT, 2 BOIL
void setMokaStatus(int status){
  changePending = true;
  mokaStatus = status;
  if(status == 1 || status == 0 || status == 3){
    brewstart = false;
    bloomfinished = false;
    bloomTime = 0;
    brewTime = 0;
  }
}
void brewMokaManual(double time){
  bloomCurrentTargetTime = time;
  setMokaStatus(2);
}
// Setpoint the PID is currently driving to (bloom or boiling target while
// extracting, heating target while preheating).
float getPidTarget(){
  return target;
}

float getTargetBoilingTemp(){
    return boil;
  }
float getTargetHeatingTemp(){
    return heat;
  }
void setTargetBoilingTemp(float temp){
  boil = temp;
  changePending = true;
  }
void setTargetHeatingTemp(float temp){
  heat = temp;
  changePending = true;
  }

void getPID(double *pid){
  pid[0] = p; pid[1] = i; pid[2] = d;
}

void getResistorValues(double *data){
  //double aux[3];
  data[0] = resistorFixed;
  //data[1] = ntcRefResistor;
  //data[2] = ntcBeta;
  data[1] = shA;
  data[2] = shB;
  data[3] = shC;
  //return aux;
}

void setPID(double newP, double newI, double newD){
  prefsLock(); MokaPreferences.begin("my-app", false);
  p = newP; i = newI; d = newD;
  savePid(newP, newI, newD);
  myPID.setGains(p,i,d);
  MokaPreferences.end(); prefsUnlock(); 
}
void setResistorValues(double newR, double newA, double newB, double newC){
  prefsLock(); MokaPreferences.begin("my-app", false);
  //resistorFixed = newR; ntcRefResistor = newN; ntcBeta = newB;
  shA = newA; shB = newB; shC = newC;
  saveResistorValues(newR, newA, newB, newC);
  MokaPreferences.end(); prefsUnlock(); 
}

// Ki scheduled with the heat loss, which grows with the difference between
// the target and the room: ~0.7 was measured stable at a delta of ~70 C.
#define KI_PER_DEGREE 0.01

// Applies the scheduled Ki to the PID only: the saved/configured i (shown and
// saved by the settings page) is left untouched.
void estimateIntegral(){
  double delta = target - getAmbientTemp();
  if(delta < 0) delta = 0;
  myPID.setGains(p, KI_PER_DEGREE * delta, d);
}