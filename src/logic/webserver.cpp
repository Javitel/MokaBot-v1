#include "webserver.hpp"
#include "WebServer.h"
#include <WiFi.h>
#include <WiFiProvisioner.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "logic/prefsLock.hpp"
#include <LovyanGFX.hpp>
#include <iostream>
#include <string.h>
#include "ui.h"
#include "logic/moka.hpp"
#include "logic/pressureSensor.hpp"
#include "logic/ota.hpp"
#include "logic/api.hpp"
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
        prefsLock(); preferencesWS.begin("my-app", false);
        if(WiFi.isConnected()){
      preferencesWS.putString("SSID", ssid);
    }
        if (password) {
          //Serial.printf("Password: %s\n", password);
          preferencesWS.putString("Password", password);
        }
        preferencesWS.end(); prefsUnlock();
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

  // Read the credentials and close the store right away: the provisioning
  // portal below can run for minutes and must not keep settings locked.
  prefsLock(); preferencesWS.begin("my-app", false);
  String ssid = preferencesWS.getString("SSID", "");
  String pass = preferencesWS.getString("Password", "");
  preferencesWS.end(); prefsUnlock();

  bool connected = !(ssid.equals("") && pass.equals("")) && connectWiFi(ssid, pass);
  // The provisioning portal blocks until it connects; repeat only if it returns
  // without a connection, unless the user cancelled it.
  provisioningCancelled = false;
  while(!connected){
    WiFi.disconnect();
    ip = "Provisioning...";
    provisioningWifi();
    if(provisioningCancelled){
      WiFi.disconnect();
      ip = "Disconnected";
      return -1;
    }
    connected = WiFi.status() == WL_CONNECTED;
  }

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
void sendSettings() {
  sendEmbeddedPage(settings_html_start, settings_html_end);
}

// Query arguments of the web server's current request.
class WebServerArgs : public ApiArgs {
public:
  bool has(const char *name) const override { return server->hasArg(name); }
  String get(const char *name) const override { return server->arg(name); }
};

// Every data endpoint (see api.hpp) is answered by apiHandle().
static void handleApi() {
  WebServerArgs args;
  ApiResponse response;
  String method = server->method() == HTTP_POST ? "POST" : "GET";
  // WebServer keeps a non-form request body in the "plain" argument.
  if (apiHandle(method, server->uri(), args, server->arg("plain"), response)) {
    server->send(response.status, response.contentType, response.body);
  }
  else {
    server->send(404, "text/plain", "Not found");
  }
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
  for (int k = 0; API_GET_PATHS[k]; k++) server->on(API_GET_PATHS[k], HTTP_GET, handleApi);
  for (int k = 0; API_POST_PATHS[k]; k++) server->on(API_POST_PATHS[k], HTTP_POST, handleApi);
  server->on("/settings", HTTP_GET, sendSettings);
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
  prefsLock(); preferencesWS.begin("my-app", false);
  preferencesWS.putString("SSID", "");
  preferencesWS.putString("Password", "");
   preferencesWS.end(); prefsUnlock();
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
