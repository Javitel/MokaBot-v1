#include <stdbool.h>

// WiFi setup network the phone joins (WiFiProvisioner portal).
#define SETUP_AP_NAME "MokaBot-Setup"
#define NET_TEXT_BLUETOOTH "Bluetooth"
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
// Text of the screen's network button ("Bluetooth", "Connecting...",
// "WiFi <address>", "WiFi setup"); short enough not to scroll.
const char * getIp();
// Why the WiFi setup portal is open (for the box on the screen); "" when it
// is not open.
const char * wifiSetupReason(void);
// Saved WiFi network name ("" when none), for C code.
void getWifiSsidText(char *buffer, int size);
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