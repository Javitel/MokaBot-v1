#include "webserver.hpp"
#include "WebServer.h"
#include <WiFi.h>
#include <WiFiProvisioner.h>
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
#include "logic/mdnsResponder.hpp"
#include "logic/discovery.hpp"
#include "logic/bleLink.hpp"
#include "logic/httpsProxy.hpp"
#include <Update.h>
#define HTTP_PORT 80
// Device name: reachable as http://<name>.local (mDNS) and, on routers that
// register DHCP hostnames, as http://<name>. Set from the settings page and
// saved in Preferences; DEFAULT_HOSTNAME until then.
#define DEFAULT_HOSTNAME "mokabot"
#define HOSTNAME_MAX     31
#define HOSTNAME_KEY     "hostname"

// WebServer::begin() fails silently if bind() fails (e.g. port 80 not yet
// released by the provisioning portal); this exposes whether it is listening.
class MokaWebServer : public WebServer {
public:
  using WebServer::WebServer;
  bool isListening() { return (bool)_server; }
};

static char deviceName[HOSTNAME_MAX + 1] = DEFAULT_HOSTNAME;
static bool deviceNameLoaded = false;
// Set when the name changes: the web server task restarts the mDNS responder
// (setDeviceName() may run on another task, e.g. the Bluetooth link).
static volatile bool mdnsRestartPending = false;

// A DNS label: letters, digits and '-', not first or last, at most 31.
static bool validDeviceName(const char *name){
  size_t length = strlen(name);
  if(length == 0 || length > HOSTNAME_MAX || name[0] == '-' || name[length - 1] == '-') return false;
  for(size_t k = 0; k < length; k++){
    if(!isalnum((unsigned char)name[k]) && name[k] != '-') return false;
  }
  return true;
}

WiFiProvisioner provisioner;
MokaWebServer *server = nullptr;
Preferences preferencesWS;
// ---- Network button text -----------------------------------------------------
// Written by the web server task, read by the screen's label task: a fixed
// buffer copied under a lock (an Arduino String reassigned on one task while
// another reads it could be freed under the reader).
#define NET_TEXT_MAX 48
static char netText[NET_TEXT_MAX] = NET_TEXT_BLUETOOTH;
static char netTextCopy[NET_TEXT_MAX];
static portMUX_TYPE netTextLock = portMUX_INITIALIZER_UNLOCKED;

static void setNetText(const String &text){
  portENTER_CRITICAL(&netTextLock);
  strlcpy(netText, text.c_str(), sizeof(netText));
  portEXIT_CRITICAL(&netTextLock);
}

// Why the WiFi setup portal is open, for the box on the screen ("" when it
// is not, see wifiSetupReason()).
static char setupReason[64] = "";

// Last reason the station was disconnected (WiFi event), to tell a wrong
// password from a network that is out of reach.
static volatile int lastDisconnectReason = 0;
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
  // The phone joins this network and sees a page to pick the home WiFi.
  WiFiProvisioner::Config &config = provisioner.getConfig();
  config.AP_NAME = SETUP_AP_NAME;
  config.HTML_TITLE = "MokaBot WiFi setup";
  config.THEME_COLOR = "#6d4c41";
  config.PROJECT_TITLE = "MokaBot";
  config.PROJECT_SUB_TITLE = "WiFi setup";
  config.PROJECT_INFO = "Choose your home WiFi and enter its password. MokaBot works with 2.4 GHz WiFi only.";
  config.FOOTER_TEXT = "MokaBot";
  config.CONNECTION_SUCCESSFUL = "MokaBot is connected. Join your home WiFi again on this phone.";
  config.SHOW_INPUT_FIELD = false; // No additional input field
  config.SHOW_RESET_FIELD = false; // No reset field

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

// Joining the home WiFi: routers can take several seconds to accept a device
// (2.5 s, as before, often fell back to the setup portal, which looked like
// the WiFi being forgotten). Up to WIFI_CONNECT_TIMEOUT_MS, then once more.
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_CONNECT_ATTEMPTS   2

static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info){
  if(event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) lastDisconnectReason = info.wifi_sta_disconnected.reason;
}

// Short explanation of the last failed connection, for the screen.
static const char *connectFailure(){
  switch(lastDisconnectReason){
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "wrong password?";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
      return "network not found (2.4 GHz?)";
    default:
      return "no answer";
  }
}

static bool connectWiFi(const String &ssid, const String &pass){
  static bool eventRegistered = false;
  if(!eventRegistered){
    WiFi.onEvent(onWifiEvent);
    eventRegistered = true;
  }
  for(int attempt = 0; attempt < WIFI_CONNECT_ATTEMPTS; attempt++){
    setNetText("Connecting...");
    lastDisconnectReason = 0;
    WiFi.begin(ssid.c_str(), pass.c_str());
    for(uint32_t waited = 0; waited < WIFI_CONNECT_TIMEOUT_MS; waited += 100){
      if(WiFi.status() == WL_CONNECTED) return true;
      // A wrong password does not get better by waiting.
      if(lastDisconnectReason == WIFI_REASON_AUTH_FAIL) break;
      delay(100);
    }
    if(WiFi.status() == WL_CONNECTED) return true;
    Serial.printf("WiFi: could not join %s (reason %d)\n", ssid.c_str(), lastDisconnectReason);
    WiFi.disconnect();
    delay(500);
  }
  return false;
}

// WiFi and Bluetooth never run together (every board): the screen's network
// button toggles between them. Starting WiFi stops Bluetooth; switching WiFi
// off frees the WiFi driver's memory and starts Bluetooth again.
static void wifiStarting(){
  bleStop();
}

static void wifiStopped(){
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  setNetText(NET_TEXT_BLUETOOTH);
  setupReason[0] = '\0';
  bleStart();
}

int initNet() {
  if(WiFi.isConnected()){
    wifiStopped();
    return 0;
  }

  wifiStarting();
  // Must be set before the station starts to be used in the DHCP request.
  WiFi.setHostname(getDeviceName());

  // Read the credentials and close the store right away: the provisioning
  // portal below can run for minutes and must not keep settings locked.
  prefsLock(); preferencesWS.begin("my-app", false);
  String ssid = preferencesWS.getString("SSID", "");
  String pass = preferencesWS.getString("Password", "");
  preferencesWS.end(); prefsUnlock();

  bool saved = !ssid.equals("");
  bool connected = saved && connectWiFi(ssid, pass);
  // Shown in the setup box on the screen: why the portal opened.
  if(!connected){
    if(saved) snprintf(setupReason, sizeof(setupReason), "Could not join %s: %s", ssid.c_str(), connectFailure());
    else strlcpy(setupReason, "No WiFi network saved yet.", sizeof(setupReason));
  }
  // The provisioning portal blocks until it connects; repeat only if it returns
  // without a connection, unless the user cancelled it.
  provisioningCancelled = false;
  while(!connected){
    WiFi.disconnect();
    setNetText("WiFi setup");
    // The portal needs the radio and memory to itself (see bleStop()).
    bleStop();
    Serial.printf("Provisioning: free heap %u\n", (unsigned)ESP.getFreeHeap());
    provisioningWifi();
    if(provisioningCancelled){
      wifiStopped();
      return -1;
    }
    connected = WiFi.status() == WL_CONNECTED;
  }
  setupReason[0] = '\0';

  serverStart();
  return 0;
}

bool isMokaConnected(){
  return WiFi.isConnected();
}

// Web pages from data/, gzip-compressed (gzip-pages.py) and compiled into the
// firmware (board_build.embed_files in platformio.ini) so OTA updates them
// together with the code. Binary embeds are not null-terminated: each has
// start/end symbols.
extern const char index_html_start[] asm("_binary__pio_pages_index_html_gz_start");
extern const char index_html_end[] asm("_binary__pio_pages_index_html_gz_end");
extern const char brew_html_start[] asm("_binary__pio_pages_brew_html_gz_start");
extern const char brew_html_end[] asm("_binary__pio_pages_brew_html_gz_end");
extern const char settings_html_start[] asm("_binary__pio_pages_settings_html_gz_start");
extern const char settings_html_end[] asm("_binary__pio_pages_settings_html_gz_end");
extern const char touch_icon_start[] asm("_binary_data_apple_touch_icon_png_start");
extern const char touch_icon_end[] asm("_binary_data_apple_touch_icon_png_end");

// "Add to Home Screen" (manifest for Android/Chrome; iOS reads the page's
// apple-touch-icon and apple-mobile-web-app-* tags).
static const char WEB_APP_MANIFEST[] =
  "{\"name\":\"MokaBot\",\"short_name\":\"MokaBot\",\"start_url\":\"/\","
  "\"display\":\"standalone\",\"background_color\":\"#ffffff\",\"theme_color\":\"#6d4c41\","
  "\"icons\":[{\"src\":\"/apple-touch-icon.png\",\"sizes\":\"180x180\",\"type\":\"image/png\"}]}";

// The browser unpacks the page (Content-Encoding: gzip).
static void sendEmbeddedPage(const char *start, const char *end) {
  server->sendHeader("Content-Encoding", "gzip");
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
static void sendTouchIcon() {
  server->sendHeader("Cache-Control", "max-age=86400");
  server->send_P(200, "image/png", touch_icon_start, touch_icon_end - touch_icon_start);
}
static void sendManifest() {
  server->send(200, "application/manifest+json", WEB_APP_MANIFEST);
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
  server->on("/apple-touch-icon.png", HTTP_GET, sendTouchIcon);
  server->on("/manifest.json", HTTP_GET, sendManifest);
  server->on("/firmware", HTTP_POST, handleFirmwareUploadDone, handleFirmwareUploadData);

  if(beginServer()){
    IPAddress localIp = WiFi.localIP();
    setNetText("WiFi " + localIp.toString());
    // http://mokabot.local (our own minimal responder, see mdnsResponder.hpp).
    const uint8_t address[4] = { localIp[0], localIp[1], localIp[2], localIp[3] };
    mdnsResponderStart(getDeviceName(), address);
    // The Android app's "Search" finds MokaBots on the network (discovery.hpp).
    discoveryStart();
    // https://<name>.local too, for browsers that insist on HTTPS (httpsProxy.hpp).
    httpsStart();
    while(getMokaWifiStatus() == 0){
      server->handleClient();
      discoveryPoll();
      if(mdnsRestartPending){
        mdnsRestartPending = false;
        mdnsResponderStart(getDeviceName(), address);
      }
      mdnsResponderPoll();
      vTaskDelay((75) / portTICK_PERIOD_MS);
    }
  }
  else{
    setNetText("Server error");
  }

  // Release the port so the next start (or provisioning) can bind it.
  httpsStop();
  mdnsResponderStop();
  discoveryStop();
  server->stop();
  delete server;
  server = nullptr;
}

void resetWiFi(){
  wifiStopped();
  prefsLock(); preferencesWS.begin("my-app", false);
  preferencesWS.putString("SSID", "");
  preferencesWS.putString("Password", "");
   preferencesWS.end(); prefsUnlock();
}

const char * getDeviceName(void){
  if(!deviceNameLoaded){
    prefsLock(); preferencesWS.begin("my-app", true);
    String saved = preferencesWS.getString(HOSTNAME_KEY, DEFAULT_HOSTNAME);
    preferencesWS.end(); prefsUnlock();
    if(validDeviceName(saved.c_str())) strlcpy(deviceName, saved.c_str(), sizeof(deviceName));
    deviceNameLoaded = true;
  }
  return deviceName;
}

bool setDeviceName(const char *name){
  char lower[HOSTNAME_MAX + 1];
  size_t length = strlen(name);
  if(length > HOSTNAME_MAX) return false;
  for(size_t k = 0; k <= length; k++) lower[k] = tolower((unsigned char)name[k]);
  if(!validDeviceName(lower)) return false;
  if(strcmp(lower, getDeviceName()) == 0) return true;
  prefsLock(); preferencesWS.begin("my-app", false);
  preferencesWS.putString(HOSTNAME_KEY, lower);
  preferencesWS.end(); prefsUnlock();
  strlcpy(deviceName, lower, sizeof(deviceName));
  // .local answers the new name right away; the router learns it (DHCP) on
  // the next WiFi connection.
  mdnsRestartPending = true;
  bleSetName(deviceName);
  Serial.printf("Device name: %s.local\n", deviceName);
  return true;
}

String getWifiSsid(){
  prefsLock(); preferencesWS.begin("my-app", true);
  String ssid = preferencesWS.getString("SSID", "");
  preferencesWS.end(); prefsUnlock();
  return ssid;
}

void setWifiCredentials(const char *ssid, const char *password){
  prefsLock(); preferencesWS.begin("my-app", false);
  String stored = preferencesWS.getString("SSID", "");
  if(ssid && stored != ssid){
    preferencesWS.putString("SSID", ssid);
    // Another network: its password, or none (open network) when not given.
    preferencesWS.putString("Password", password ? password : "");
  }
  else if(password){
    preferencesWS.putString("Password", password);
  }
  preferencesWS.end(); prefsUnlock();
}

const char * getIp() {
  portENTER_CRITICAL(&netTextLock);
  memcpy(netTextCopy, netText, sizeof(netTextCopy));
  portEXIT_CRITICAL(&netTextLock);
  return netTextCopy;
}

const char * wifiSetupReason(void){
  return provisioning ? setupReason : "";
}

void getWifiSsidText(char *buffer, int size){
  strlcpy(buffer, getWifiSsid().c_str(), size);
}
int getMokaWifiStatus(){
  return mokaWifiStatus;
}

void setMokaWifiStatus(int status){
  mokaWifiStatus =status;
}
