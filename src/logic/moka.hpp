#ifdef __cplusplus
extern "C" {
#endif


void init(void);
void pidUpdate(void);
void updateSensors(void *param);
// Task: reads the ambient pressure and temperature (BMP280) periodically.
void getAmbientData(void *param);
void process(void *param);
void brewTimer(void *param);   
void updateTemp(void);
void updateTempNTC(void);
void averageTemp();
float getCurrentTemp();
int getMokaStatus();
//0 OFF, 1 HEAT, 2 BOIL, 3 MANUAL POWER
void setMokaStatus(int status);
// Manual power mode: relay on for `percent` (0-100) of each PID time window,
// without the PID. manualPowerOn() also switches to status 3.
void manualPowerOn(int percent);
void setManualPower(int percent);
int getManualPower();
// Degrees above the boiling point at which manual power switches off
// (negative = below). Saved in Preferences.
void setManualCutoff(int degreesAboveBoiling);
int getManualCutoff();
void brewMokaManual(double time);
void setBloomTime(double overrideTime);
void setBloomOffset(double overrideTime);
// Setpoint the PID is currently driving to.
float getPidTarget();
float getTargetBoilingTemp();
float getTargetHeatingTemp();
void setTargetBoilingTemp(float temp);
void setTargetHeatingTemp(float temp);
void getPID(double *pid);
// Fills data[0..5]: fixed R, NTC R, NTC beta, Steinhart-Hart A, B, C.
void getResistorValues(double *data);
double readPid();
void setPID(double newP, double newI, double newD);
void setResistorValues(double newR, double newA, double newB, double newC);
double getBloomTime();
double getBrewTime();
double getBloomOffset();
double getBloomTargetTime();
void setManualBloomTime(double overrideTime);
void skipBloom();
void estimateIntegral();
   #ifdef __cplusplus
      } // extern "C"
   #endif