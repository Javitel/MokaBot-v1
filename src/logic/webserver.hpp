#include <stdbool.h>

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