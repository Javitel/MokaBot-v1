#ifdef __cplusplus
extern "C" {
#endif

void initSensor();
float getCurrentPressure();
float getAmbientTemp();
void updateCurrentPressure();
void calibrateBoiling();
void overrideBoiling(double input);
float getCalibratedTemp();
double getCalibratedPressure();
void saveTargetHeatingTemp(float temp);
void saveTargetBoilingTemp(float temp);
float getBoilingTemp(float pressure);
float getPressureByTemp(float temp, float pressure);
   #ifdef __cplusplus
      } // extern "C"
   #endif