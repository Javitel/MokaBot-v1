#include "webserver.hpp"
#include "WebServer.h"
#include <WiFiProvisioner.h>
//#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include <Preferences.h>
#include <LovyanGFX.hpp>
#include <iostream>
#include <string.h>
#include "ui.h"
#include "logic/moka.hpp"
#include "logic/pressureSensor.hpp"
#include "logic/pro.hpp"

WiFiProvisioner provisioner;
WebServer *server;
Preferences preferencesWS;
String ip = "Disconnected";
int mokaWifiStatus= 0;

String retrieveData(){
  float internalPressure;
  if(!isnan(getCurrentTemp())){
    internalPressure = getPressureByTemp(getCurrentTemp(),getCurrentPressure());
  }
  else{
    internalPressure = getCurrentPressure()/100000;
  }
    String aux = (String)getCurrentTemp()+ "_" +(String)internalPressure ;
  //Serial.println(aux);
  return aux;
}

String retrieveExtractionData(){
  float internalPressure;
  if(!isnan(getCurrentTemp())){
    internalPressure = getPressureByTemp(getCurrentTemp(),getCurrentPressure());
  }
  else{
    internalPressure = getCurrentPressure()/100000;
  }
    String aux = (String)getCurrentTemp()+ "_" +(String)internalPressure + "_" + (String)getBrewTime()+ "_" + (String)getMokaStatus();
  //Serial.println(aux);
  return aux;
}

String retrievePID(){
  String aux = "";
  double current[3];
  getPID(current);
  char str[150];
  sprintf(str, "%f", current[0]);
  aux = aux + str+"_";
  sprintf(str, "%f", current[1]);
  aux = aux + str+"_";
  sprintf(str, "%f", current[2]);
  aux = aux + str+"_";
  getResistorValues(current);
  sprintf(str, "%f", current[0]);
  aux = aux + str+"_";
  sprintf(str, "%f", current[1]);
  aux = aux + str+"_";
  sprintf(str, "%f", current[2]);
  aux = aux + str;
  return aux;
}
String retrieveResistor(){
  String aux = "";
  double current[3];
  getResistorValues(current);
  char str[100];
  sprintf(str, "%f", current[0]);
  aux = aux + str+"_";
  sprintf(str, "%f", current[1]);
  aux = aux + str+"_";
  sprintf(str, "%f", current[2]);
  aux = aux + str;
  return aux;
}

String retrieveBoilInfo(){
  String aux = "";
  double current[3];
  //getResistorValues(current);
  char str[100];
  sprintf(str, "%f", getBloomTargetTime());
  aux = aux + str+"_";
  sprintf(str, "%f", getBoilingTemp(getCurrentPressure()/100000));
  aux = aux + str+"_";
  sprintf(str, "%f", getBloomOffset());
  aux = aux + str;
  return aux;
}

void provisioningWifi(){
  
  provisioner.getConfig().SHOW_INPUT_FIELD = false; // No additional input field
    provisioner.getConfig().SHOW_RESET_FIELD = false; // No reset field

  // Set the success callback
    provisioner.onSuccess(
      [](const char *ssid, const char *password, const char *input) {
        //Serial.printf("Provisioning successful! Connected to SSID: %s\n", ssid);
        if(WiFi.isConnected()){
      preferencesWS.putString("SSID", ssid);
    }
        if (password) {
          //Serial.printf("Password: %s\n", password);
          preferencesWS.putString("Password", password);
        }
      }
    );

  // Start provisioning
    provisioner.startProvisioning();
}

void webserverService(void *param){
  
  while(true){
    //vTaskDelay((250) / portTICK_PERIOD_MS);
    //Toggle, switch
    if (getMokaWifiStatus() == 1){
      setMokaWifiStatus(0);
      initNet();

    }
    //Reset Wifi config
    if(getMokaWifiStatus() == -1){
      setMokaWifiStatus(0);
      resetWiFi();
    }
    vTaskDelay( pdMS_TO_TICKS( 250 ));
  }
}

int initNet() {
  if(WiFi.isConnected()){
    WiFi.disconnect();
    //lv_timer_handler(); 
    //mokaWifiStatus = 0;
    ip = "Disconnected";
    return 0;
  }else{
    server = new WebServer(8080);
  preferencesWS.begin("my-app", false); 
  String ssid = preferencesWS.getString("SSID", "");
  String pass = preferencesWS.getString("Password", "");
  if(pass.equals("") && ssid.equals("")){
    ip = "Provisioning...";
    //mokaWifiStatus =-2;
    provisioningWifi();
    //return -1;
  }
  else{
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
    int retries = 0;
    while(WiFi.status() != WL_CONNECTED){
        //Serial.print(".");
        delay(100);
        retries ++;
        if(retries > 25){
          WiFi.disconnect();
          ip = "Provisioning...";
          provisioningWifi();
          //return -1;
        }
    }

  preferencesWS.end();
    if(!SPIFFS.begin(true)){
    }
 serverStart();
 return 0;
}
  
}

bool isMokaConnected(){
  return WiFi.isConnected();
}

void sendIndex() {
  File index = SPIFFS.open("/index.html", "r");
  server->send(200, "text/html", index.readString());
}
void sendBrew() {
  File index = SPIFFS.open("/brew.html", "r");
  server->send(200, "text/html", index.readString());
}
void sendData() {
  //File index = SPIFFS.open("/index.html", "r");
  server->send(200, "text/plain", retrieveData());
}
void sendBrewData() {
  //File index = SPIFFS.open("/index.html", "r");
  server->send(200, "text/plain", retrieveExtractionData());
}
void sendPID() {
  //File index = SPIFFS.open("/index.html", "r");
  server->send(200, "text/plain", retrievePID());
}
void sendResistorValues() {
  //File index = SPIFFS.open("/index.html", "r");
  server->send(200, "text/plain", retrieveResistor());
}

void sendBoilValues() {
  //File index = SPIFFS.open("/index.html", "r");
  server->send(200, "text/plain", retrieveBoilInfo());
}
void sendSettings() {
  File settings = SPIFFS.open("/settings.html", "r");
  server->send(200, "text/html", settings.readString());
}
void getBloomInfo() {
  //File index = SPIFFS.open("/index.html", "r");
  server->send(200, "text/plain", "0");
}
void setBloomInfo() {
  String BloomTime;
  String BoilTemp;
  String BloomOffset;
  double current[3];
  //getResistorValues(current);
  if(server->hasArg("BloomTime")){
    BloomTime = server->arg("BloomTime");
      if(BloomTime.toDouble() >= 0.0){
        setBloomTime(BloomTime.toDouble());
        //setResistorValues(current[0],current[1], current[2]);
      }
      else {
      BloomTime = "";
    }
  }
  if(server->hasArg("BoilTemp")){
    BoilTemp = server->arg("BoilTemp");
      if(BoilTemp.toDouble() >= 0.0){
        overrideBoiling(BoilTemp.toDouble());
        //current[1]=N.toDouble();
        //setResistorValues(current[0],current[1], current[2]);
      }
      else {
      BoilTemp = "";
    }
  }
  if(server->hasArg("BloomOffset")){
    BloomOffset = server->arg("BloomOffset");
      if(BloomOffset.toDouble() >= -20.0){
        setBloomOffset(BloomOffset.toDouble());
        //current[2]=B.toDouble();
        //setResistorValues(current[0],current[1], current[2]);
      }
      else {
      BloomOffset = "";
    }
  }
  sendSettings();
}
void setResistors() {
  String R;
  String N;
  String B;
  double current[3];
  getResistorValues(current);
  if(server->hasArg("RVal")){
    R = server->arg("RVal");
      if(R.toDouble() >= 0.0){
        current[0]=R.toDouble();
        setResistorValues(current[0],current[1], current[2]);
      }
      else {
      R = "";
    }
  }
  if(server->hasArg("NVal")){
    N = server->arg("NVal");
      if(N.toDouble() >= 0.0){
        current[1]=N.toDouble();
        setResistorValues(current[0],current[1], current[2]);
      }
      else {
      N = "";
    }
  }
  if(server->hasArg("BVal")){
    B = server->arg("BVal");
      if(B.toDouble() >= 0.0){
        current[2]=B.toDouble();
        setResistorValues(current[0],current[1], current[2]);
      }
      else {
      B = "";
    }
  }
  sendSettings();
}
void handleUpdate() {
  //File index = SPIFFS.open("/index.html", "r");
  String P;
  String I;
  String D;
  if(server->hasArg("PVal")){
    P = server->arg("PVal");
      double current[3];
       getPID(current);
      if(P.toDouble() >= 0.0){
        current[0]=P.toDouble();
        setPID(current[0],current[1], current[2]);
      }
      else {
      P = "";
    }
  }
  if(server->hasArg("IVal")){
    I = server->arg("IVal");
      double current[3];
       getPID(current);
      if(I.toDouble() >= 0.0){
        current[1]=I.toDouble();
        setPID(current[0],current[1], current[2]);
      }
      else {
      I = "";
    }
  }
  if(server->hasArg("DVal")){
    D = server->arg("DVal");
      double current[3];
       getPID(current);
      if(D.toDouble() >= 0.0){
        current[2]=D.toDouble();
        setPID(current[0],current[1], current[2]);
      }
      else {
      D = "";
    }
  }
  sendSettings();
}
void tareScale() {
  //File index = SPIFFS.open("/index.html", "r");
  tare(1);
  sendSettings();
}
void calibrateScale() {
  //File index = SPIFFS.open("/index.html", "r");
  String Calibrate;
  if(server->hasArg("calibrate")){
    Calibrate = server->arg("calibrate");
    calibrateCellSensor(Calibrate.toDouble());
  }
  sendSettings();
}

void serverStart(){
 //server.reset();

  server->on("/debug", HTTP_GET, sendIndex);
  server->on("/", HTTP_GET, sendBrew);
  server->on("/temperature", HTTP_GET, sendData);
  server->on("/brewInfo", HTTP_GET, sendBrewData);
  server->on("/tare", HTTP_GET, tareScale);
  //server->on("/calibrate", HTTP_GET, calibrateScale);
  server->on("/pidInfo", HTTP_GET, sendPID);
  server->on("/settings", HTTP_GET, sendSettings);
  server->on("/set", HTTP_GET, handleUpdate);
  server->on("/boilInfo", HTTP_GET, sendBoilValues);
  server->on("/resistorInfo", HTTP_GET, sendResistorValues);
  server->on("/setResistor", HTTP_GET, setResistors);
  server->on("/setBrewData", HTTP_GET, setBloomInfo);
  server->on("/set", HTTP_GET, getBloomInfo);
  
//Serial.print("Server Starting")
  server->begin();
  ip = WiFi.localIP().toString();
//ip = WiFi.localIP().toString();
while(getMokaWifiStatus() == 0){
        server->handleClient();
        vTaskDelay((75) / portTICK_PERIOD_MS);
      }
    
  }

void resetWiFi(){
  WiFi.disconnect();
  ip = "Disconnected";
  preferencesWS.begin("my-app", false);
  preferencesWS.putString("SSID", "");
  preferencesWS.putString("Password", "");
   preferencesWS.end();
}

const char * getIp() {
  return ip.c_str();
}
int getMokaWifiStatus(){
  return mokaWifiStatus;
}

void setMokaWifiStatus(int status){
  mokaWifiStatus =status;
}