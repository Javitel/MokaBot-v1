#include "webserver.hpp"
#include "WebServer.h"
#include <WiFi.h>
#include <WiFiProvisioner.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LovyanGFX.hpp>
#include <iostream>
#include <string.h>
#include "ui.h"
#include "logic/moka.hpp"
#include "logic/pressureSensor.hpp"
#include "logic/ota.hpp"
#include "logic/brewHistory.hpp"
#include <Update.h>
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
// Appends value as a JSON string literal (quotes, backslashes and control
// characters escaped).
static void appendJsonString(String &json, const String &value) {
  json += '"';
  for (size_t k = 0; k < value.length(); k++) {
    char c = value[k];
    if (c == '"' || c == '\\') { json += '\\'; json += c; }
    else if ((unsigned char)c < 0x20) { char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", c); json += esc; }
    else json += c;
  }
  json += '"';
}

static void appendJson(String &json, const char *name, double value, int decimals = 6) {
  char str[64];
  if (isnan(value)) snprintf(str, sizeof(str), "\"%s\":null", name);
  else if (decimals < 0) snprintf(str, sizeof(str), "\"%s\":%.10g", name, value);
  else snprintf(str, sizeof(str), "\"%s\":%.*f", name, decimals, value);
  if (json.length() > 1) json += ",";
  json += str;
}

static float currentInternalPressure(){
  return brewInternalPressure();
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
  appendJson(json, "ManualCutoff", getManualCutoff(), 0);
  appendJson(json, "Fahrenheit", ui_use_fahrenheit() ? 1 : 0, 0);
  appendJson(json, "Decimals", ui_show_decimals() ? 1 : 0, 0);
  char hex[32];
  snprintf(hex, sizeof(hex), ",\"ButtonColor\":\"#%06lx\"", (unsigned long)ui_get_button_color());
  json += hex;
  json += ",\"OtaUrl\":";
  appendJsonString(json, otaGetUrl());
  json += ",\"OtaFile\":";
  appendJsonString(json, otaFirmwareFile());
  json += "}";
  return json;
}

// ---- Cancelling provisioning ----------------------------------------------
// WiFiProvisioner has no public abort: its blocking loop() only ends when the
// private _serverLoopFlag is set, and then releases the portal (web server,
// DNS, AP) itself. Explicit template instantiation is allowed to name private
// members, which gives a pointer-to-member to that flag without patching the
// library.
template <typename Tag, typename Tag::type Member>
struct PrivateMemberAccess {
  friend typename Tag::type privateMember(Tag) { return Member; }
};
struct ProvisionerLoopFlag {
  typedef bool WiFiProvisioner::*type;
  friend type privateMember(ProvisionerLoopFlag);
};
template struct PrivateMemberAccess<ProvisionerLoopFlag, &WiFiProvisioner::_serverLoopFlag>;

static volatile bool provisioning = false;
static volatile bool provisioningCancelled = false;

bool isProvisioning(){
  return provisioning;
}

// Called from the UI: makes the provisioning loop exit and clean up.
void stopProvisioning(){
  if(!provisioning) return;
  provisioningCancelled = true;
  provisioner.*privateMember(ProvisionerLoopFlag()) = true;
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

  // Start provisioning (blocks until it connects or stopProvisioning() is called)
    provisioning = true;
    provisioner.startProvisioning();
    provisioning = false;
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
  // without a connection, unless the user cancelled it.
  provisioningCancelled = false;
  while(!connected){
    WiFi.disconnect();
    ip = "Provisioning...";
    provisioningWifi();
    if(provisioningCancelled){
      preferencesWS.end();
      WiFi.disconnect();
      ip = "Disconnected";
      return -1;
    }
    connected = WiFi.status() == WL_CONNECTED;
  }
  preferencesWS.end();

  serverStart();
  return 0;
}

bool isMokaConnected(){
  return WiFi.isConnected();
}

// Web pages from data/, compiled into the firmware (board_build.embed_files
// in platformio.ini) so OTA updates them together with the code. Binary
// embeds are not null-terminated: each has start/end symbols.
extern const char index_html_start[] asm("_binary_data_index_html_start");
extern const char index_html_end[] asm("_binary_data_index_html_end");
extern const char brew_html_start[] asm("_binary_data_brew_html_start");
extern const char brew_html_end[] asm("_binary_data_brew_html_end");
extern const char settings_html_start[] asm("_binary_data_settings_html_start");
extern const char settings_html_end[] asm("_binary_data_settings_html_end");

static void sendEmbeddedPage(const char *start, const char *end) {
  server->send_P(200, "text/html", start, end - start);
}

void sendIndex() {
  sendEmbeddedPage(index_html_start, index_html_end);
}
void sendBrew() {
  sendEmbeddedPage(brew_html_start, brew_html_end);
}
void sendData() {
  server->send(200, "application/json", retrieveData());
}
// GET /brewHistory?session=S&from=N: the recorded brew samples (all of them
// for a new session, otherwise only those from index N).
void sendBrewHistory() {
  long session = server->hasArg("session") ? server->arg("session").toInt() : -1;
  long from = server->hasArg("from") ? server->arg("from").toInt() : 0;
  server->send(200, "application/json", brewHistoryJson(session, from));
}
void sendBrewData() {
  server->send(200, "application/json", retrieveExtractionData());
}
void sendSettingsValues() {
  server->send(200, "application/json", retrieveSettings());
}
void sendSettings() {
  sendEmbeddedPage(settings_html_start, settings_html_end);
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
  // Any integer, negative or positive.
  if (readArg("ManualCutoff", -1e9, value)) setManualCutoff((int)lround(value));

  // UI
  uint32_t buttonColor;
  if (readHexColorArg("ButtonColor", buttonColor)) ui_set_button_color(buttonColor);
  if (readArg("Fahrenheit", 0.0, value)) ui_set_use_fahrenheit(value != 0);
  if (readArg("Decimals", 0.0, value)) ui_set_show_decimals(value != 0);

  // OTA
  if (server->hasArg("OtaUrl")) otaSetUrl(server->arg("OtaUrl"));

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

// ---- Firmware upload from the settings page --------------------------------
// The .bin is streamed straight into the inactive OTA partition while it is
// received, then the board reboots into it.
static bool firmwareUploadOk = false;

static void handleFirmwareUploadData(){
  HTTPUpload &upload = server->upload();
  if(upload.status == UPLOAD_FILE_START){
    firmwareUploadOk = false;
    // Nothing should heat while the firmware is being replaced.
    setMokaStatus(0);
    Serial.printf("Firmware upload: %s\n", upload.filename.c_str());
    if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  }
  else if(upload.status == UPLOAD_FILE_WRITE){
    if(Update.isRunning() && Update.write(upload.buf, upload.currentSize) != upload.currentSize){
      Update.printError(Serial);
    }
  }
  else if(upload.status == UPLOAD_FILE_END){
    firmwareUploadOk = Update.end(true);
    if(!firmwareUploadOk) Update.printError(Serial);
  }
  else if(upload.status == UPLOAD_FILE_ABORTED){
    Update.abort();
  }
}

static void handleFirmwareUploadDone(){
  if(firmwareUploadOk){
    server->send(200, "text/plain", "Firmware updated, rebooting");
    delay(500); // let the response reach the browser
    ESP.restart();
  }
  else{
    server->send(500, "text/plain", String("Update failed: ") + Update.errorString());
  }
}

void serverStart(){
  server = new MokaWebServer(HTTP_PORT);
  server->on("/debug", HTTP_GET, sendIndex);
  server->on("/", HTTP_GET, sendBrew);
  server->on("/temperature", HTTP_GET, sendData);
  server->on("/brewInfo", HTTP_GET, sendBrewData);
  server->on("/brewHistory", HTTP_GET, sendBrewHistory);
  server->on("/settingsInfo", HTTP_GET, sendSettingsValues);
  server->on("/settings", HTTP_GET, sendSettings);
  server->on("/set", HTTP_GET, handleUpdate);
  server->on("/firmware", HTTP_POST, handleFirmwareUploadDone, handleFirmwareUploadData);

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
