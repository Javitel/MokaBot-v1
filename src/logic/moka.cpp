#include <Arduino.h>
#include <Wire.h> 
#include <Adafruit_ADS1X15.h>
#include <AutoPID.h>
#include "moka.hpp"
#include <Preferences.h>
#include <time.h>
#include "logic/pressureSensor.hpp"

const int RelayActivationValue = HIGH;
//const int pinDataDQ = 33;
const int pinHeater = 32;
//const int pinNTC = 27;

boolean running = false;
boolean changePending = false;

Adafruit_ADS1115 ads;

double gain=1.0;

double heat = 92.0;
double boil = 106.0;
double target =0.0;
double temperature;
double bloomDefaultTargetTime = 0;
double bloomCurrentTargetTime = 0;
double bloomOffset = 0;
boolean bloomfinished = false;
boolean brewstart = false;
double bloomTime = 0;
double brewTime = 0;
double startTime = 0;
double elapsedTime = 0;

double refVoltage = 0;
double resistorFixed = 2000;
double ntcRefResistor=50000;
double ntcBeta=3950;
double ntcValue = 0;

double current = 0.0;
int mokaStatus = 0;
bool connected=false;
bool relayControl;

double defaultp = 47.5;//0.03 0.003;
double defaulti = 0.00075;//0.00045;//0.0004;
double defaultd = 0.085;//0.000002;

double p, i, d;  
// AutoPIDRelay myPID(&temperature, &target, &relayControl, 250, defaultp, defaulti, defaultd);

double timeWindow =250;
double pidValue;
AutoPID myPID(&temperature, &target, &pidValue, 0, timeWindow, defaultp, defaulti, defaultd);

Preferences MokaPreferences;

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
    uint32_t average = 0;
    uint32_t aux = 0;
    ads.startADCReading(id, /*continuous=*/true);
    vTaskDelay( pdMS_TO_TICKS( 50));//delay(10);
    boolean retry =false;
    for (uint8_t i = 0; i < samples; i++) {
      //aux = ads.readADC_SingleEnded(id);
      aux = ads.getLastConversionResults();
      //Serial.println(aux);
      //if(ads.conversionComplete()){
        if((aux > ref + margin || aux < ref - margin) && ref > 0 && retry == false){
          i--;
          retry = true;
        }
        else{
          retry = false;
          average += aux;
        }
      //}
      //else{
      //  i--;
      //}
      
      vTaskDelay( pdMS_TO_TICKS( 10));//delay(10);
    }
    Serial.print("result  average: ");
    Serial.println(average/samples);
    return average/samples;
  }

void updateSensors(void *param){
  
   //ads.startADCReading(0, true);
     TickType_t xLastWakeTime;
const TickType_t xFrequency = pdMS_TO_TICKS(150);
BaseType_t xWasDelayed;
  while(true){
      try{
      refVoltage = ads.computeVolts(ads.readADC_SingleEnded(3));
      updateTempNTC();
      } catch (...){}
    //vTaskDelay(pdMS_TO_TICKS(50));
    xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
  }
}

void updateAmbientPressure(void *param){

    // Initialise the xLastWakeTime variable with the current time.
TickType_t xLastWakeTime;
const TickType_t xFrequency = pdMS_TO_TICKS(1000);
BaseType_t xWasDelayed;
  while(true){
        updateCurrentPressure();
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
  ntcRefResistor = MokaPreferences.getFloat("ntcRefResistor", ntcRefResistor);
  ntcBeta = MokaPreferences.getFloat("ntcBeta", ntcBeta);
}

void recoverBrewSettings(){
  bloomDefaultTargetTime = MokaPreferences.getFloat("bloomTime", bloomDefaultTargetTime);
  bloomCurrentTargetTime = MokaPreferences.getFloat("bloomTime", bloomCurrentTargetTime);
  bloomOffset = MokaPreferences.getFloat("bloomOffset", bloomOffset);

}

void savePid(double newP, double newI, double newD){
  MokaPreferences.putFloat("savedP", newP);
  MokaPreferences.putFloat("savedI", newI);
  MokaPreferences.putFloat("savedD", newD);
}
void saveResistorValues(double newR, double newN, double newB){
  MokaPreferences.putFloat("resistorFixed", newR);
  MokaPreferences.putFloat("ntcRefResistor", newN);
  MokaPreferences.putFloat("ntcBeta", newB);
}

void init() {
Wire.setTimeOut(25);
myPID.setBangBang(8,1);
pinMode(pinHeater, OUTPUT);
digitalWrite(pinHeater,getrelayStatus(LOW));
ads.setGain(GAIN_TWOTHIRDS);
ads.setDataRate(RATE_ADS1115_860SPS);
ads.begin();
MokaPreferences.begin("my-app", false);
recoverPid();
recoverResistors();
recoverBrewSettings();
MokaPreferences.end(); 
myPID.setGains(p,i,d);
}

void process(void *param){
 // TickType_t startTime = xTaskGetTickCount();
  double aux;
  TickType_t xLastWakeTime;
const TickType_t xFrequency = pdMS_TO_TICKS(255);
BaseType_t xWasDelayed;
  while(true){
//vTaskDelay((150) / portTICK_PERIOD_MS);
    //updateCurrentPressure();
     if(current <= -127.0 || mokaStatus == 0){
      connected=false;
      digitalWrite(pinHeater,getrelayStatus(LOW));
      //vTaskDelay( pdMS_TO_TICKS( 50 ));
      //elapsedTime += (millis() - startTime);
    }
    else{  
      connected=true;
        if(!running){
    pidUpdate();
    if(getMokaStatus() == 2 && brewstart == false && (current + bloomOffset) >= getCalibratedTemp()){
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
    }
  }

  }
      if(getMokaStatus() == 2 && brewstart == true){
      //aux = xTaskGetTickCount() - startTime;
      aux = millis() - startTime + elapsedTime;
      
      if(bloomTime >= bloomCurrentTargetTime || bloomfinished == true){
        bloomfinished = true;
        target = boil;
      }
      else{bloomTime = aux/1000;}
      brewTime = aux/1000;
    }
xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
    //vTaskDelay( pdMS_TO_TICKS( 30 ));
}

}

void setBloomTime(double overrideTime){
    MokaPreferences.begin("my-app", false);
    bloomCurrentTargetTime = overrideTime;
    bloomDefaultTargetTime= overrideTime;
    MokaPreferences.putFloat("bloomTime", bloomDefaultTargetTime);
    MokaPreferences.end();
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
void setBloomOffset(double overrideTemp){
    MokaPreferences.begin("my-app", false);
    bloomOffset = overrideTemp;
    MokaPreferences.putFloat("bloomOffset", bloomOffset);
    MokaPreferences.end();
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

void updateTempNTC(){
  refVoltage = averageADSRead(ADS1X15_REG_CONFIG_MUX_SINGLE_3,3, 25, refVoltage);//ads.readADC_Differential_1_3();//
  //refVoltage= refVoltage * 0.1875/1000;  f
  refVoltage = ads.computeVolts(refVoltage);
  double temp;
   //float refMultiplier = 0.1875/1000;
 //957.0;//10000;//9980.0;
 double beta = ntcBeta;
double resistance;
 double lnR;
 double kelvin;
 double vm = 0.0;
 double aux = 0;
  //double A=0.9668452613/1000;
  //double B=1.964261376/10000;
  //double C=2.099773678/10000000;

  //BEST SO FAR
  // double A=0.6622698572/1000;
  // double B=2.487175948/10000;
  // double C=0.004697715028/10000000;
//sub 1c boiling
// double A=0.7367564649/1000;
//   double B=2.377184165/10000;
//   double C=0.3581657140/10000000;

  double A=0.9032071364/1000;
  double B=2.086434938/10000;
  double C=1.597510723/10000000;
  ntcValue = averageADSRead(ADS1X15_REG_CONFIG_MUX_SINGLE_0,8, 75, ntcValue);//ads.readADC_Differential_0_3();//
  Serial.print(refVoltage);
Serial.print("-");
//vm=( ntcValue ) * 0.1875/1000;     
vm = ads.computeVolts(ntcValue);
Serial.print(vm);
Serial.print("-");
//resistance = resistorFixed / ((refVoltage/vm)-1); 
resistance = (refVoltage - vm)*(resistorFixed/vm);

Serial.print(resistance);
Serial.print("-");
//kelvin = beta/(log(resistance/ntcRefResistor)+(beta/298.15));
//kelvin =1.0/kelvin;

//Steinhart-Hart equation
//lnR = log(resistance/vm);
aux = A + (B * log(resistance)) + (C * pow(log(resistance), 3));
kelvin =1.0/aux;

temp = kelvin- 273.15;
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
  if(status == 1 || status == 0){
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
  data[1] = ntcRefResistor;
  data[2] = ntcBeta;
  //return aux;
}

void setPID(double newP, double newI, double newD){
  MokaPreferences.begin("my-app", false);
  p = newP; i = newI; d = newD;
  savePid(newP, newI, newD);
  myPID.setGains(p,i,d);
  MokaPreferences.end(); 
}
void setResistorValues(double newR, double newN, double newB){
  MokaPreferences.begin("my-app", false);
  resistorFixed = newR; ntcRefResistor = newN; ntcBeta = newB;
  saveResistorValues(newR, newN, newB);
  MokaPreferences.end(); 
}