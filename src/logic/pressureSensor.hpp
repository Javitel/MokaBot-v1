#ifdef __cplusplus
extern "C" {
#endif

void initSensor();
float getCurrentPressure();
void updateCurrentPressure();
// Measures the boiling point (maximum temperature over 3 s). Only runs in
// preheat with a target of at least 100 C; results outside 85-105 C are
// rejected. Non-blocking.
void calibrateBoiling();
// Short status of the last calibration for the screen ("Calibrating...",
// "Boiling: ", "Needs preheat >="...), "" when nothing to show. *temp gets the
// temperature in Celsius to append after the text, or NAN when there is none.
const char * calibrationMessage(float *temp);
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