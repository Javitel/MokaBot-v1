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

   #ifdef __cplusplus
      } // extern "C"
   #endif