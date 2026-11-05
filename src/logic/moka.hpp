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
//0 OFF, 1 HEAT, 2 BOIL, 3 MANUAL POWER, 4 PID CALIBRATION
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
// Preheat is limited to the boiling point + this offset (-10..0 C, default
// -2). Saved in Preferences.
void setPreheatOffset(double degrees);
double getPreheatOffset();
// An extraction (normal or profile) ends when, after reaching its target, the temperature
// goes this far above it (water running low). 0.5..10 C, default 2. Saved in
// Preferences.
void setOvershootMargin(double degrees);
double getOvershootMargin();
// Why the last extraction (normal or profile) ended by itself, until the next
// status change: BREW_STOP_NONE, BREW_STOP_OVERSHOOT (above target + overshoot
// margin) or BREW_STOP_LOW_WATER (boiler heating much faster than at the
// start, see the low water detection in moka.cpp).
#define BREW_STOP_NONE      0
#define BREW_STOP_OVERSHOOT 1
#define BREW_STOP_LOW_WATER 2
int getBrewStopReason();

// PID calibration (status 4): from water at most 10 C above the room, full
// power; the heating rate and dead time give P, I and D, refined over three
// 1.5 C steps up to boiling point + offset (less overshoot, faster settling);
// the best ones are saved once the target holds within +-0.4 C for 60 s.
#define PID_TUNE_IDLE     0
#define PID_TUNE_HEATING  1   // full power, measuring
#define PID_TUNE_SETTLING 2   // new gains holding the target
#define PID_TUNE_DONE     3   // saved
#define PID_TUNE_FAILED   4   // cancelled / timed out / implausible: old gains kept
// Start condition: OFF, valid reading, water at most 10 C above the room.
bool pidTuneReady();
bool pidTuneStart();
void pidTuneCancel();
int pidTunePhase();
const char * pidTuneMessage();
float pidTuneTarget();
// Calibration target = boiling point + this (-15..0, default -5). Saved.
void setPidTuneOffset(double degrees);
double getPidTuneOffset();
// Heater on-time fraction (0..1) of the last PID / manual power window.
float getHeaterDuty();
// Heating gain now / at the start of the extraction (1 = as full; higher =
// less water); NAN until known.
float getWaterGainRatio();
// Gain ratio that ends an extraction (1.2..5, 0 = off, the default). Saved
// in Preferences.
void setLowWaterRatio(double ratio);
double getLowWaterRatio();
// Highest preheat target: boiling point (current pressure) + preheat offset,
// at most 110 C.
float getHeatingLimit();
// Boiling calibration: preheat (status 1) towards 110 C, past the preheat
// limit. Any status change ends it.
void calibrationHeatingOn();
void calibrationHeatingOff();
bool isCalibrationHeating();
void brewMokaManual(double time);
// Extraction driven by a saved profile: the PID follows the target given with
// setProfileTarget() (no bloom/boiling targets) until the status changes.
// Refuses to start without a valid temperature reading.
void profileBrewOn(float firstTarget);
void setProfileTarget(float temp);
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
// Removes the stored P, I and D: the firmware's defaults apply (now and after
// updates that change them).
void resetPidDefaults();
void getDefaultPID(double *pid);
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