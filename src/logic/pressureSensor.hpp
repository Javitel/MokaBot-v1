#ifdef __cplusplus
extern "C" {
#endif

void initSensor();
float getCurrentPressure();
void updateCurrentPressure();
// Measures the boiling point: preheats towards 110 C (past the preheat limit)
// until the temperature rises less than 0.5 C in 3 s, and saves the highest
// reading of those 3 s. Starts from OFF or preheat, from 5 C below the theoretical boiling
// point at the altitude (ambient pressure); results above 105 C are
// rejected. Non-blocking; any status change cancels it.
void calibrateBoiling();
// Short status of the last calibration for the screen ("Calibrating...",
// "Boiling: ", "Calibration cancelled"...), "" when nothing to show. *temp gets the
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