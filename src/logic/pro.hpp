#ifdef __cplusplus
extern "C" {
#endif

int isBaseConnected();
double getBaseTemp();
double getWeight();
void proRoutine(void *param);
void tare(int longPress);
void calibrateCellSensor(double weigth);
   #ifdef __cplusplus
      } // extern "C"
   #endif