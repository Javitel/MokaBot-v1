#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int initNet();
void resetWiFi();
const char * getIp();
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