#include "ota.hpp"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include "logic/moka.hpp"
#include "taskCores.hpp"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// Firmware file for this board, appended to the configured base URL, so one
// server folder can hold the images of every board. Normally defined by
// ota-images.py from `custom_ota_firmware` in platformio.ini (the same name
// the build copies firmware.bin to); these are fallbacks.
#ifndef OTA_FIRMWARE_FILE
#if defined(BOARD_WAVESHARE_S3_TOUCH_LCD_35B)
#define OTA_FIRMWARE_FILE "waveshare-s3-touch-lcd-3.5b.bin"
#else
#define OTA_FIRMWARE_FILE "wt32-sc01.bin"
#endif
#endif

#define OTA_PREFS_NAMESPACE "my-app"
#define OTA_URL_KEY         "otaUrl"
// How long the final status (error / up to date) stays on screen.
#define OTA_RESULT_SHOW_MS  5000

static volatile bool otaRunning = false;
static volatile bool otaShowing = false;
static volatile int otaPercent = -1;
static char availableVersion[24] = "";
static char otaStatus[32] = "";

static void setStatus(const char *text){
  strlcpy(otaStatus, text, sizeof(otaStatus));
}

String otaGetUrl(){
  Preferences prefs;
  prefs.begin(OTA_PREFS_NAMESPACE, true);
  String url = prefs.getString(OTA_URL_KEY, "");
  prefs.end();
  return url;
}

void otaSetUrl(const String &url){
  Preferences prefs;
  prefs.begin(OTA_PREFS_NAMESPACE, false);
  prefs.putString(OTA_URL_KEY, url);
  prefs.end();
}

const char * otaFirmwareFile(){
  return OTA_FIRMWARE_FILE;
}

// Base URL (folder) from the settings + this board's firmware file.
static String otaFirmwareUrl(){
  String base = otaGetUrl();
  if(base.length() == 0) return base;
  if(!base.endsWith("/")) base += "/";
  return base + OTA_FIRMWARE_FILE;
}

// <base URL>/<firmware file without .bin>.version
static String otaVersionUrl(){
  String url = otaFirmwareUrl();
  if(url.endsWith(".bin")) url.remove(url.length() - 4);
  return url + ".version";
}

// GETs a small text file; true with its trimmed content on HTTP 200.
static bool fetchText(const String &url, String &out){
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  bool begun;
  if(url.startsWith("https://")){
    secure.setInsecure();
    begun = http.begin(secure, url);
  }
  else{
    begun = http.begin(plain, url);
  }
  if(!begun) return false;
  int code = http.GET();
  if(code == HTTP_CODE_OK){
    out = http.getString();
    out.trim();
  }
  http.end();
  return code == HTTP_CODE_OK && out.length() > 0;
}

// Compares dotted versions numerically ("1.10.0" > "1.9.2"): <0, 0 or >0.
static int compareVersions(const String &a, const String &b){
  int ia = 0, ib = 0;
  while(ia < (int)a.length() || ib < (int)b.length()){
    long na = 0, nb = 0;
    while(ia < (int)a.length() && a[ia] != '.') { if(isdigit((unsigned char)a[ia])) na = na * 10 + (a[ia] - '0'); ia++; }
    while(ib < (int)b.length() && b[ib] != '.') { if(isdigit((unsigned char)b[ib])) nb = nb * 10 + (b[ib] - '0'); ib++; }
    if(na != nb) return na < nb ? -1 : 1;
    ia++; ib++;
  }
  return 0;
}

static void otaTask(void *param){
  String url = otaFirmwareUrl();

  if(!WiFi.isConnected()){
    setStatus("OTA: no WiFi");
  }
  else if(url.length() == 0){
    setStatus("OTA: no URL set");
  }
  else{
    setStatus("OTA: checking");
    String versionUrl = otaVersionUrl();
    String remoteVersion;
    bool knowsVersion = fetchText(versionUrl, remoteVersion);
    strlcpy(availableVersion, knowsVersion ? remoteVersion.c_str() : "unknown", sizeof(availableVersion));
    Serial.printf("OTA: installed %s, available %s (%s)\n", FIRMWARE_VERSION, availableVersion, versionUrl.c_str());

    if(knowsVersion && compareVersions(remoteVersion, FIRMWARE_VERSION) <= 0){
      setStatus("OTA: up to date");
      goto finished;
    }

    setStatus("OTA: downloading");
    Serial.printf("OTA: downloading %s\n", url.c_str());
    // Nothing should heat while the firmware is being replaced.
    setMokaStatus(0);

    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress([](int current, int total){
      int percent = total > 0 ? (int)(current * 100LL / total) : 0;
      otaPercent = percent;
      char text[sizeof(otaStatus)];
      snprintf(text, sizeof(text), "OTA: %d%%", percent);
      setStatus(text);
    });

    t_httpUpdate_return result;
    if(url.startsWith("https://")){
      // No certificate pinning: the server is trusted as configured.
      WiFiClientSecure client;
      client.setInsecure();
      result = httpUpdate.update(client, url, FIRMWARE_VERSION);
    }
    else{
      WiFiClient client;
      result = httpUpdate.update(client, url, FIRMWARE_VERSION);
    }

    // On success the board has already rebooted; only the other results
    // get here.
    if(result == HTTP_UPDATE_NO_UPDATES){
      setStatus("OTA: up to date");
    }
    else{
      Serial.printf("OTA failed (%d): %s\n", httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      setStatus("OTA: failed");
    }
  }

finished:
  otaPercent = -1;
  otaRunning = false;
  vTaskDelay(pdMS_TO_TICKS(OTA_RESULT_SHOW_MS));
  if(!otaRunning) otaShowing = false;
  vTaskDelete(NULL);
}

void otaStartCheck(void){
  if(otaRunning) return;
  otaRunning = true;
  otaShowing = true;
  otaPercent = -1;
  availableVersion[0] = '\0';
  setStatus("OTA: starting");
  // Network work, next to the WiFi stack; HTTPS needs a large stack.
  if(xTaskCreatePinnedToCore(otaTask, "ota", 8192, NULL, 2, NULL, CORE_I2C) != pdPASS){
    // No task to clear the flags later: do it here so the popup closes.
    Serial.println("OTA: could not start the update task");
    otaRunning = false;
    otaShowing = false;
  }
}

bool otaInProgress(void){
  return otaShowing;
}

const char * otaStatusText(void){
  return otaStatus;
}

int otaProgress(void){
  return otaPercent;
}

const char * otaInstalledVersion(void){
  return FIRMWARE_VERSION;
}

const char * otaAvailableVersion(void){
  return availableVersion;
}
