#include "webserver.hpp"
#include "WebServer.h"
#include <WiFiProvisioner.h>
#include "SPIFFS.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LovyanGFX.hpp>
#include <iostream>
#include <string.h>
#include "ui.h"
#include "logic/moka.hpp"
#include "logic/pressureSensor.hpp"
#define HTTP_PORT 80
// Reachable as http://mokabot.local (mDNS) and, on routers that register DHCP
// hostnames, as http://mokabot.
#define HOSTNAME "mokabot"

// WebServer::begin() fails silently if bind() fails (e.g. port 80 not yet
// released by the provisioning portal); this exposes whether it is listening.
class MokaWebServer : public WebServer {
public:
  using WebServer::WebServer;
  bool isListening() { return (bool)_server; }
};

WiFiProvisioner provisioner;
MokaWebServer *server = nullptr;
Preferences preferencesWS;
String ip = "Disconnected";
int mokaWifiStatus= 0;

// Appends "name":value to a JSON object body; NaN becomes null.
// decimals < 0 uses %g with 10 significant digits (for very small values).
static void appendJson(String &json, const char *name, double value, int decimals = 6) {
  char str[64];
  if (isnan(value)) snprintf(str, sizeof(str), "\"%s\":null", name);
  else if (decimals < 0) snprintf(str, sizeof(str), "\"%s\":%.10g", name, value);
  else snprintf(str, sizeof(str), "\"%s\":%.*f", name, decimals, value);
  if (json.length() > 1) json += ",";
  json += str;
}

static float currentInternalPressure(){
  if(!isnan(getCurrentTemp())){
    return getPressureByTemp(getCurrentTemp(),getCurrentPressure());
  }
  return getCurrentPressure()/100000;
}

// {"temp":<C>,"pressure":<bar>}
String retrieveData(){
  String json = "{";
  appendJson(json, "temp", getCurrentTemp());
  appendJson(json, "pressure", currentInternalPressure());
  json += "}";
  return json;
}

// {"temp":<C>,"pressure":<bar>,"time":<brew seconds>,"status":<moka status>}
String retrieveExtractionData(){
  String json = "{";
  appendJson(json, "temp", getCurrentTemp());
  appendJson(json, "pressure", currentInternalPressure());
  appendJson(json, "time", getBrewTime());
  appendJson(json, "status", getMokaStatus());
  json += "}";
  return json;
}

// All settings values as JSON, keyed by the settings form input ids.
String retrieveSettings(){
  String json = "{";
  double current[6];
  getPID(current);
  appendJson(json, "PVal", current[0]);
  appendJson(json, "IVal", current[1]);
  appendJson(json, "DVal", current[2]);
  getResistorValues(current);
  appendJson(json, "RVal", current[0]);
  appendJson(json, "SHA", current[1], -1);
  appendJson(json, "SHB", current[2], -1);
  appendJson(json, "SHC", current[3], -1);
  appendJson(json, "BloomTime", getBloomTargetTime());
  appendJson(json, "BoilTemp", getBoilingTemp(getCurrentPressure()/100000), 2);
  appendJson(json, "BloomOffset", getBloomOffset());
  appendJson(json, "Fahrenheit", ui_use_fahrenheit() ? 1 : 0, 0);
  appendJson(json, "Decimals", ui_show_decimals() ? 1 : 0, 0);
  char hex[32];
  snprintf(hex, sizeof(hex), ",\"ButtonColor\":\"#%06lx\"", (unsigned long)ui_get_button_color());
  json += hex;
  json += "}";
  return json;
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

// Tries to connect with the given credentials for ~2.5s.
static bool connectWiFi(const String &ssid, const String &pass){
  WiFi.begin(ssid.c_str(), pass.c_str());
  for(int retries = 0; retries < 25; retries++){
    if(WiFi.status() == WL_CONNECTED) return true;
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

int initNet() {
  if(WiFi.isConnected()){
    WiFi.disconnect();
    ip = "Disconnected";
    return 0;
  }

  // Must be set before the station starts to be used in the DHCP request.
  WiFi.setHostname(HOSTNAME);

  preferencesWS.begin("my-app", false);
  String ssid = preferencesWS.getString("SSID", "");
  String pass = preferencesWS.getString("Password", "");

  bool connected = !(ssid.equals("") && pass.equals("")) && connectWiFi(ssid, pass);
  // The provisioning portal blocks until it connects; repeat only if it returns
  // without a connection.
  while(!connected){
    WiFi.disconnect();
    ip = "Provisioning...";
    provisioningWifi();
    connected = WiFi.status() == WL_CONNECTED;
  }
  preferencesWS.end();

  if(!SPIFFS.begin(true)){
  }
  serverStart();
  return 0;
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
  server->send(200, "application/json", retrieveData());
}
void sendBrewData() {
  server->send(200, "application/json", retrieveExtractionData());
}
void sendSettingsValues() {
  server->send(200, "application/json", retrieveSettings());
}
void sendSettings() {
  File settings = SPIFFS.open("/settings.html", "r");
  server->send(200, "text/html", settings.readString());
}
// Reads a "#rrggbb" / "rrggbb" query argument. Returns false if missing or invalid.
static bool readHexColorArg(const char *name, uint32_t &out) {
  if (!server->hasArg(name)) return false;
  String value = server->arg(name);
  if (value.startsWith("#")) value = value.substring(1);
  if (value.length() != 6) return false;
  for (size_t k = 0; k < value.length(); k++) {
    if (!isxdigit((unsigned char)value[k])) return false;
  }
  out = strtoul(value.c_str(), nullptr, 16);
  return true;
}

// Reads a numeric query argument. Returns false if missing, empty or below minVal.
static bool readArg(const char *name, double minVal, double &out) {
  if (!server->hasArg(name)) return false;
  String value = server->arg(name);
  if (value.length() == 0) return false;
  double parsed = value.toDouble();
  if (parsed < minVal) return false;
  out = parsed;
  return true;
}

void handleUpdate() {
  double value;

  // PID
  double pid[3];
  getPID(pid);
  bool pidChanged = false;
  pidChanged |= readArg("PVal", 0.0, pid[0]);
  pidChanged |= readArg("IVal", 0.0, pid[1]);
  pidChanged |= readArg("DVal", 0.0, pid[2]);
  if (pidChanged) setPID(pid[0], pid[1], pid[2]);

  // Resistors
  double resistors[6];
  getResistorValues(resistors);
  bool resistorsChanged = false;
  resistorsChanged |= readArg("RVal", 0.0, resistors[0]);
  resistorsChanged |= readArg("SHA", -1.0, resistors[1]);
  resistorsChanged |= readArg("SHB", -1.0, resistors[2]);
  resistorsChanged |= readArg("SHC", -1.0, resistors[3]);
  if (resistorsChanged) setResistorValues(resistors[0], resistors[1], resistors[2], resistors[3]);

  // Bloom / boil
  if (readArg("BloomTime", 0.0, value)) setBloomTime(value);
  if (readArg("BoilTemp", 0.0, value)) overrideBoiling(value);
  if (readArg("BloomOffset", -20.0, value)) setBloomOffset(value);

  // UI
  uint32_t buttonColor;
  if (readHexColorArg("ButtonColor", buttonColor)) ui_set_button_color(buttonColor);
  if (readArg("Fahrenheit", 0.0, value)) ui_set_use_fahrenheit(value != 0);
  if (readArg("Decimals", 0.0, value)) ui_set_show_decimals(value != 0);

  server->send(200, "text/plain", "OK");
}

// Starts the HTTP server, retrying until port 80 is free; the provisioning
// portal may still be releasing it right after provisioning finishes.
static bool beginServer(){
  for(int attempt = 0; attempt < 20; attempt++){
    server->begin();
    if(server->isListening()) return true;
    server->close(); // closes the socket a failed bind() leaves open
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  return false;
}

void serverStart(){
  server = new MokaWebServer(HTTP_PORT);
  server->on("/debug", HTTP_GET, sendIndex);
  server->on("/", HTTP_GET, sendBrew);
  server->on("/temperature", HTTP_GET, sendData);
  server->on("/brewInfo", HTTP_GET, sendBrewData);
  server->on("/settingsInfo", HTTP_GET, sendSettingsValues);
  server->on("/settings", HTTP_GET, sendSettings);
  server->on("/set", HTTP_GET, handleUpdate);

  if(beginServer()){
    ip = WiFi.localIP().toString();
    if(MDNS.begin(HOSTNAME)){
      MDNS.addService("http", "tcp", HTTP_PORT);
    }
    while(getMokaWifiStatus() == 0){
      server->handleClient();
      vTaskDelay((75) / portTICK_PERIOD_MS);
    }
  }
  else{
    ip = "Server error";
  }

  // Release the port so the next start (or provisioning) can bind it.
  MDNS.end();
  server->stop();
  delete server;
  server = nullptr;
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
