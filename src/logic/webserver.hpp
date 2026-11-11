#include <stdbool.h>
#ifdef __cplusplus
#include <Arduino.h>
// Saved WiFi network name (empty when none).
String getWifiSsid();
#endif

#ifdef __cplusplus
extern "C" {
#endif

int initNet();
void resetWiFi();
const char * getIp();
// Device name: http://<name>.local and the DHCP hostname ("mokabot" by default).
const char * getDeviceName(void);
// Letters, digits and '-' (not first or last), up to 31; saved lowercase.
// False (and unchanged) when invalid.
bool setDeviceName(const char *name);
int checkStoredCreds();
// Saved WiFi credentials, used the next time WiFi is switched on. ssid null:
// keep the network; password null: keep the password, unless the network
// changes (then no password, for an open network).
void setWifiCredentials(const char *ssid, const char *password);
void webserverService(void *param);
void provisionWifiConnection();
void serverStart();
int getMokaWifiStatus();
void setMokaWifiStatus(int status);
bool isMokaConnected();
// True while the WiFi provisioning portal is running.
bool isProvisioning();
// Stops the provisioning portal (no-op when it is not running).
void stopProvisioning();

   #ifdef __cplusplus
      } // extern "C"
   #endif