#ifdef __cplusplus
extern "C" {
#endif

void initSensor();
float getCurrentPressure();
void updateCurrentPressure();
void calibrateBoiling();
void overrideBoiling(double input);
float getCalibratedTemp();
double getCalibratedPressure();
void saveTargetHeatingTemp(float temp);
void saveTargetBoilingTemp(float temp);
float getBoilingTemp(float pressure);
float getPressureByTemp(float temp, float pressure);
// Last ambient temperature read by updateAmbientTemp() (no I2C: safe from
// any task).
float getAmbientTemp();
// Reads the BMP280 temperature into the value getAmbientTemp() returns.
void updateAmbientTemp();
   #ifdef __cplusplus
      } // extern "C"
   #endif