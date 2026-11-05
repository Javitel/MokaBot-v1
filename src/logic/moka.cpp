#include <Arduino.h>
#include <Wire.h> 
//#include <Adafruit_ADS1X15.h>
#include "ADS1X15.h"
#include <AutoPID.h>
#include "moka.hpp"
#include <Preferences.h>
#include "logic/prefsLock.hpp"
#include <time.h>
#include "logic/pressureSensor.hpp"
#include "devices/boardPins.hpp"
#include "taskCores.hpp"

const int RelayActivationValue = HIGH;
//const int pinDataDQ = 33;
const int pinHeater = PIN_HEATER;
//const int pinNTC = 27;

boolean running = false;
boolean changePending = false;

//Adafruit_ADS1115 ads;
ADS1115 ads(0x48);

double gain=1.0;

double heat = 92.0;
double boil = 106.0;
double target =0.0;
double temperature;
double bloomDefaultTargetTime = 0;
double bloomCurrentTargetTime = 0;
double bloomOffset = 0;
// Manual power mode switches itself off this many degrees above the boiling
// point (may be negative). Saved in Preferences, set from the settings page.
int manualCutoffAboveBoiling = 3;
// Accepted range for manualCutoffAboveBoiling (C relative to boiling).
#define MANUAL_CUTOFF_MIN -10
#define MANUAL_CUTOFF_MAX 10

// Hard safety limit for every mode: above this temperature the heater is
// switched off and the moka goes OFF, whatever the target or cut-off is
// (a high target or a typo in the settings must not boil the pot dry or drive
// it to its safety valve). Above the highest target (MAX_BOILING_TARGET) and
// below the hardware thermostat, which cuts the heater at 140 C.
#define MAX_SAFE_TEMP 135.0
// Upper limits for the targets (set with +/- and loaded from settings).
#define MAX_HEATING_TARGET 110.0
#define MAX_BOILING_TARGET 125.0

// Preheat never goes above the boiling point (at the current pressure) plus
// this offset, so the water does not boil before extracting. Saved in
// Preferences, set from the settings page; only zero or negative.
double preheatOffset = -2;
#define PREHEAT_OFFSET_MIN -10.0
#define PREHEAT_OFFSET_MAX 0.0

// Extraction (normal or profile): once the target has been reached
// (targetReached), a temperature more than this above it means the boiler is
// running low on water (less water, the PID overshoots) and the brew ends.
// Saved in Preferences, set from the settings page.
double overshootMargin = 2;
#define OVERSHOOT_MARGIN_MIN 0.5
#define OVERSHOOT_MARGIN_MAX 10.0

static double clampOvershootMargin(double degrees){
  if(degrees < OVERSHOOT_MARGIN_MIN) return OVERSHOOT_MARGIN_MIN;
  if(degrees > OVERSHOOT_MARGIN_MAX) return OVERSHOOT_MARGIN_MAX;
  return degrees;
}

static double clampPreheatOffset(double degrees){
  if(degrees < PREHEAT_OFFSET_MIN) return PREHEAT_OFFSET_MIN;
  if(degrees > PREHEAT_OFFSET_MAX) return PREHEAT_OFFSET_MAX;
  return degrees;
}

static int clampManualCutoff(int degrees){
  if(degrees < MANUAL_CUTOFF_MIN) return MANUAL_CUTOFF_MIN;
  if(degrees > MANUAL_CUTOFF_MAX) return MANUAL_CUTOFF_MAX;
  return degrees;
}
boolean bloomfinished = false;
boolean brewstart = false;
double bloomTime = 0;
double brewTime = 0;
double startTime = 0;
double elapsedTime = 0;

double refVoltage = 0;
double resistorFixed = 2000;
//double ntcRefResistor=50000;
//double ntcBeta=3950;
// Steinhart-Hart coefficients of the NTC.
double shA=0.9032071364/1000;
double shB=2.086434938/10000;
double shC=1.597510723/10000000;
double ntcValue = 0;

double current = 0.0;
int mokaStatus = 0;
bool connected=false;

// Defaults from a PID calibration on the WT32-SC01 moka (heating rate 0.412
// C/s, dead time 22.8 s; held 95 C within +-0.1 C without overshoot, D 0).
double defaultp = 13.31;
// I setting: integral gain per degree of (target - room temperature); the
// PID's Ki is scheduled as i * delta (see estimateIntegral).
double defaulti = 0.00109;
double defaultd = 0;

// Earlier defaults {P, I, D}. Saving the settings page stores the values it
// shows, so a device that never changed them may hold one of these: it means
// "the default" and follows the current one.
static const double OLD_DEFAULT_PIDS[][3] = {
  { 10.2, 0.006, 0.85 },     // 1.0.2 - 1.0.3
  { 40.2, 0.01, 0.085 },     // earlier builds
};

static bool sameValue(double a, double b){
  return fabs(a - b) <= 1e-6 + 1e-4 * fabs(b);
}

double p, i, d;  

double timeWindow =250;
double pidValue;
// Ki starts at 0: estimateIntegral() sets the scheduled value.
AutoPID myPID(&temperature, &target, &pidValue, 0, timeWindow, defaultp, 0, defaultd);

Preferences MokaPreferences;

// Handle of the running brewTimer task, NULL when it is not running.
static TaskHandle_t brewTimerTask = NULL;

// Boiling calibration (see calibrateBoiling): preheat drives to
// MAX_HEATING_TARGET, past the preheat limit, until the water boils.
static volatile bool calibrationHeating = false;

// Extraction driven by a saved profile (see profileBrewOn): the PID follows
// profileTarget instead of the bloom and boiling targets.
static volatile bool profileBrew = false;
static volatile double profileTarget = 0;
// End-of-brew checks of an extraction (see checkBrewEnd).
static bool targetReached = false;
static double reachedTarget = NAN;               // target targetReached refers to
static volatile int brewStopReason = BREW_STOP_NONE;   // why the last extraction ended

// ---- Low water detection --------------------------------------------------
// The boiler's heat balance: C dT/dt = P u - k (T - Ta), u = heater duty. Less
// water means a smaller heat capacity C, so the heating gain a = P/C (C/s at
// full power) grows. a is fitted continuously (recursive least squares over
// WATER_WINDOW_MS windows of dT = a * sum(u dt) - b * sum((T - Ta) dt)); its
// ratio to the value at the start of the extraction, a/a0, estimates how much
// faster the boiler heats than when it was full.
#define WATER_WINDOW_MS        5000
#define WATER_FORGETTING       0.9      // per window: about the last 50 s count
#define WATER_REF_WINDOWS      6        // windows (30 s) to learn a0 without a preheat
#define WATER_HIGH_WINDOWS     2        // ratio above the limit for 2 windows (10 s)
#define FREE_HEATING_RATE      0.4      // C/s rise...
#define FREE_HEATING_DUTY      0.2      // ...with less than 20 % duty: nothing left to heat
// Gain ratio that ends an extraction (0 = off). Saved in Preferences. Off by
// default until tuned with logged brews: in a simulation the pot's metal
// holds about as much heat as the water (an empty boiler only heats ~x2
// faster), a steadily held temperature shows no change at all, and probe lag
// makes the estimate drift.
double lowWaterRatio = 0;
#define LOW_WATER_RATIO_MIN 1.2
#define LOW_WATER_RATIO_MAX 5.0

static volatile float heaterDuty = 0;     // on-time fraction of the last window
static double waterTheta[2] = { 0, 0 };   // a, b
static double waterP[2][2];
static int waterWindows = 0;              // fitted windows since the reset
static double windowU = 0, windowL = 0, windowStartTemp = NAN, windowTarget = NAN;
static uint32_t windowStartMs = 0, windowLastMs = 0;
static double gainRef = NAN;              // a0
static double refSum = 0;
static int refCount = 0;
static int highWindows = 0;
static volatile float waterGainRatio = NAN;

// PID calibration target = boiling point + this. Saved in Preferences.
#define PID_TUNE_OFFSET_MIN  -15.0
#define PID_TUNE_OFFSET_MAX  0.0
double pidTuneOffset = -5;

static double clampPidTuneOffset(double degrees){
  if(degrees < PID_TUNE_OFFSET_MIN) return PID_TUNE_OFFSET_MIN;
  if(degrees > PID_TUNE_OFFSET_MAX) return PID_TUNE_OFFSET_MAX;
  return degrees;
}

static double clampLowWaterRatio(double ratio){
  if(ratio <= 0) return 0;
  if(ratio < LOW_WATER_RATIO_MIN) return LOW_WATER_RATIO_MIN;
  if(ratio > LOW_WATER_RATIO_MAX) return LOW_WATER_RATIO_MAX;
  return ratio;
}

// New preheat or brew: the fit starts over.
static void waterReset(){
  waterTheta[0] = waterTheta[1] = 0;
  waterP[0][0] = waterP[1][1] = 1e4;
  waterP[0][1] = waterP[1][0] = 0;
  waterWindows = 0;
  windowStartTemp = NAN;
  gainRef = NAN;
  refSum = 0;
  refCount = 0;
  highWindows = 0;
  waterGainRatio = NAN;
}

// Extraction starts: a0 is the gain fitted during the preheat, or (without
// one) the average of the first WATER_REF_WINDOWS windows.
static void waterExtractionStart(){
  gainRef = (waterWindows >= WATER_REF_WINDOWS && waterTheta[0] > 0) ? waterTheta[0] : NAN;
  refSum = 0;
  refCount = 0;
  highWindows = 0;
  waterGainRatio = NAN;
}

// One recursive least squares step: y = theta . x.
static void waterFit(double x0, double x1, double y){
  double px0 = waterP[0][0] * x0 + waterP[0][1] * x1;
  double px1 = waterP[1][0] * x0 + waterP[1][1] * x1;
  double denominator = WATER_FORGETTING + x0 * px0 + x1 * px1;
  if(denominator <= 0) return;
  double k0 = px0 / denominator, k1 = px1 / denominator;
  double error = y - (waterTheta[0] * x0 + waterTheta[1] * x1);
  waterTheta[0] += k0 * error;
  waterTheta[1] += k1 * error;
  double xp0 = x0 * waterP[0][0] + x1 * waterP[1][0];
  double xp1 = x0 * waterP[0][1] + x1 * waterP[1][1];
  waterP[0][0] = (waterP[0][0] - k0 * xp0) / WATER_FORGETTING;
  waterP[0][1] = (waterP[0][1] - k0 * xp1) / WATER_FORGETTING;
  waterP[1][0] = (waterP[1][0] - k1 * xp0) / WATER_FORGETTING;
  waterP[1][1] = (waterP[1][1] - k1 * xp1) / WATER_FORGETTING;
}

// Called after each PID window while preheating or extracting. Returns true
// (once per window) with the window's mean rise rate and duty filled in.
static bool waterEstimateStep(double *rate, double *duty){
  uint32_t now = millis();
  float ambient = getAmbientTemp();
  if(isnan(ambient) || ambient < -20 || ambient > 60) ambient = 25;
  if(isnan(windowStartTemp)){
    windowStartTemp = current;
    windowTarget = target;
    windowStartMs = windowLastMs = now;
    windowU = windowL = 0;
    return false;
  }
  double dt = (now - windowLastMs) / 1000.0;
  windowLastMs = now;
  windowU += heaterDuty * dt;
  windowL += (current - ambient) * dt;
  if(now - windowStartMs < WATER_WINDOW_MS) return false;

  double seconds = (now - windowStartMs) / 1000.0;
  double rise = current - windowStartTemp;
  // A target change (bloom -> boil, a profile step) jumps the duty: skipped.
  if(target == windowTarget){
    waterFit(windowU, -windowL, rise);
    waterWindows++;
  }
  *rate = rise / seconds;
  *duty = windowU / seconds;
  windowStartTemp = current;
  windowTarget = target;
  windowStartMs = now;
  windowU = windowL = 0;

  if(mokaStatus == 2){
    if(isnan(gainRef) && waterTheta[0] > 0 && waterWindows > 0){
      refSum += waterTheta[0];
      if(++refCount >= WATER_REF_WINDOWS) gainRef = refSum / refCount;
    }
    if(!isnan(gainRef) && gainRef > 0) waterGainRatio = waterTheta[0] / gainRef;
  }
  return true;
}

static void endBrew(int reason, const char *why){
  Serial.printf("Brew ended: %s\n", why);
  setMokaStatus(0);
  brewStopReason = reason;
}

// End of an extraction (normal or profile), checked after each PID window:
//  1. gain ratio a/a0 above lowWaterRatio for WATER_HIGH_WINDOWS windows;
//  2. free heating: rising fast with almost no heater duty;
//  3. overshoot: once the target is reached, more than overshootMargin above
//     it. A new target (bloom -> boil, a step of a profile) has to be reached
//     again; after a step down that means cooling into the band first.
// Rules 1 and 2 are off when lowWaterRatio is 0.
static void checkBrewEnd(bool windowDone, double rate, double duty){
  if(windowDone && lowWaterRatio > 0){
    float ratio = waterGainRatio;
    highWindows = (!isnan(ratio) && ratio > lowWaterRatio) ? highWindows + 1 : 0;
    if(highWindows >= WATER_HIGH_WINDOWS){
      char why[64];
      snprintf(why, sizeof(why), "low water (gain x%.1f)", ratio);
      return endBrew(BREW_STOP_LOW_WATER, why);
    }
    if(targetReached && rate > FREE_HEATING_RATE && duty < FREE_HEATING_DUTY){
      char why[64];
      snprintf(why, sizeof(why), "low water (%.2f C/s at %.0f %% duty)", rate, duty * 100);
      return endBrew(BREW_STOP_LOW_WATER, why);
    }
  }
  if(target != reachedTarget){
    targetReached = false;
    reachedTarget = target;
  }
  if(!targetReached){
    if(current >= target && current <= target + overshootMargin) targetReached = true;
  }
  else if(current > target + overshootMargin){
    char why[64];
    snprintf(why, sizeof(why), "overshoot (%.1f C > %.1f + %.1f C)", current, target, overshootMargin);
    endBrew(BREW_STOP_OVERSHOOT, why);
  }
}

int getrelayStatus(int status){
  if (status == LOW){
    if(RelayActivationValue == LOW)
      return HIGH;
    else
      return LOW;
  }
  else{
    if(RelayActivationValue == LOW)
      return LOW;
    else
      return HIGH;
  }
}

  double averageADSRead(uint16_t id, int samples, double margin, double ref){
    // Signed: readings near 0 V can be slightly negative, which wrapped the
    // old unsigned sum (e.g. "result average: 429496728").
    int32_t average = 0;
    int16_t aux = 0;
    ads.setMode(0);
    ads.requestADC(id);
    //ads.startADCReading(id, /*continuous=*/true);
    vTaskDelay( pdMS_TO_TICKS( 30));//delay(10);
    boolean retry =false;
    int valid = 0;
    for (uint8_t i = 0; i < samples; i++) {
      // Skip samples whose I2C read failed (the library reports it through
      // getError(); the catch is only a last resort).
      bool ok = false;
      try{
        aux = ads.getValue();
        ok = ads.getError() == ADS1X15_OK;
      } catch (...){}
      if(ok){
        if((aux > ref + margin || aux < ref - margin) && ref > 0 && retry == false){
          i--;
          retry = true;
        }
        else{
          retry = false;
          average += aux;
          valid++;
        }
      }

      vTaskDelay( pdMS_TO_TICKS( 17));//delay(10);
    }
    // No valid sample: NAN, the caller skips this reading.
    if(valid == 0) return NAN;
    Serial.print("result  average: ");
    Serial.println((double)average/valid);
    return (double)average/valid;
  }

void updateSensors(void *param){
  
   //ads.startADCReading(0, true);
     TickType_t xLastWakeTime = xTaskGetTickCount();
const TickType_t xFrequency = pdMS_TO_TICKS(150);
BaseType_t xWasDelayed;
  while(true){
      try{
      int16_t raw = ads.readADC(3);
      if(ads.getError() == ADS1X15_OK){
        refVoltage = ads.toVoltage(1)*raw;
      }
      updateTempNTC();
      } catch (...){}
    //vTaskDelay(pdMS_TO_TICKS(50));
    xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
  }
}

void getAmbientData(void *param){

    // Initialise the xLastWakeTime variable with the current time.
TickType_t xLastWakeTime = xTaskGetTickCount();
const TickType_t xFrequency = pdMS_TO_TICKS(2000);
BaseType_t xWasDelayed;
  while(true){
        updateCurrentPressure();
        // Ambient temperature too: the PID task reads it from a variable.
        updateAmbientTemp();
        xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
  }
}

void recoverPid(){
  p = MokaPreferences.getFloat("savedP", defaultp);
  // New key: the old "savedI" held a plain Ki (e.g. 0.00075), which as a
  // per-degree factor would almost disable the integral.
  i = MokaPreferences.getFloat("kiPerDegree", defaulti);
  d = MokaPreferences.getFloat("savedD", defaultd);
  for(size_t k = 0; k < sizeof(OLD_DEFAULT_PIDS) / sizeof(OLD_DEFAULT_PIDS[0]); k++){
    if(sameValue(p, OLD_DEFAULT_PIDS[k][0]) && sameValue(i, OLD_DEFAULT_PIDS[k][1]) &&
       sameValue(d, OLD_DEFAULT_PIDS[k][2])){
      p = defaultp;
      i = defaulti;
      d = defaultd;
      break;
    }
  }
}
void recoverResistors(){
  resistorFixed = MokaPreferences.getFloat("resistorFixed", resistorFixed);
  //ntcRefResistor = MokaPreferences.getFloat("ntcRefResistor", ntcRefResistor);
  //ntcBeta = MokaPreferences.getFloat("ntcBeta", ntcBeta);
  shA = MokaPreferences.getDouble("shA", shA);
  shB = MokaPreferences.getDouble("shB", shB);
  shC = MokaPreferences.getDouble("shC", shC);
}

void recoverBrewSettings(){
  bloomDefaultTargetTime = MokaPreferences.getFloat("bloomTime", bloomDefaultTargetTime);
  bloomCurrentTargetTime = MokaPreferences.getFloat("bloomTime", bloomCurrentTargetTime);
  bloomOffset = MokaPreferences.getFloat("bloomOffset", bloomOffset);
  // Clamped: a value saved before the limits existed may be out of range.
  manualCutoffAboveBoiling = clampManualCutoff(MokaPreferences.getInt("cutoffAbove", manualCutoffAboveBoiling));
  preheatOffset = clampPreheatOffset(MokaPreferences.getFloat("preheatOffset", preheatOffset));
  overshootMargin = clampOvershootMargin(MokaPreferences.getFloat("overshoot", overshootMargin));
  lowWaterRatio = clampLowWaterRatio(MokaPreferences.getFloat("lowWaterRatio", lowWaterRatio));
  pidTuneOffset = clampPidTuneOffset(MokaPreferences.getFloat("pidTuneOffset", pidTuneOffset));

}

void savePid(double newP, double newI, double newD){
  MokaPreferences.putFloat("savedP", newP);
  MokaPreferences.putFloat("kiPerDegree", newI);
  MokaPreferences.putFloat("savedD", newD);
}
void saveResistorValues(double newR, double newA, double newB, double newC){
  MokaPreferences.putFloat("resistorFixed", newR);
  //MokaPreferences.putFloat("ntcRefResistor", newN);
  //MokaPreferences.putFloat("ntcBeta", newB);
  // Doubles: the coefficients are tiny and need the extra precision.
  MokaPreferences.putDouble("shA", newA);
  MokaPreferences.putDouble("shB", newB);
  MokaPreferences.putDouble("shC", newC);
}

void init() {
//Wire.setTimeOut(100);
myPID.setBangBang(12,1);
pinMode(pinHeater, OUTPUT);
digitalWrite(pinHeater,getrelayStatus(LOW));
//ads.setGain(GAIN_TWOTHIRDS);
//ads.setDataRate(RATE_ADS1115_860SPS);
ads.setGain(0); 
ads.setDataRate(7); 
ads.begin();
prefsLock(); MokaPreferences.begin("my-app", false);
recoverPid();
recoverResistors();
recoverBrewSettings();
MokaPreferences.end(); prefsUnlock(); 
// Ki is scheduled from the target (0 until a mode sets one).
estimateIntegral();
}


// Boiling point (C) at the current ambient pressure; the calibrated boiling
// temperature when no valid pressure reading is available yet.
static float currentBoilingPoint(){
  float pressure = getCurrentPressure();
  if(pressure > 0) return getBoilingTemp(pressure / 100000);
  return getCalibratedTemp();
}

// Whole degrees (floored), like the targets set with +/-.
float getHeatingLimit(){
  float limit = floorf(currentBoilingPoint() + preheatOffset);
  return limit < MAX_HEATING_TARGET ? limit : MAX_HEATING_TARGET;
}

// ---- PID calibration (status 4) -------------------------------------------
// Step-response tuning (SIMC rules for an integrating process with dead time):
// from water at most PID_TUNE_START_ABOVE_ROOM above the room, the heater runs at full power (the PID is
// bang-bang more than 12 C below its target). The rise from start + 3 C to
// start + 13 C gives the heating rate R (C/s at full power) and, from where
// that line crosses the start temperature, the dead time theta (probe and
// heat-up delay). Then
//   Kc = 1 / (2 R theta)  (per C of error, as a duty fraction)
//   Ti = 8 theta
// in AutoPID's units (output = ms of the 250 ms window): P = 250 Kc,
// Ki = P / Ti, and the I setting is per degree of (target - room): I = Ki /
// (target - room). Td = theta / 2: D = P Td, times 1e6 because AutoPID
// divides its derivative by the step in ms and again by 1000.
// Refinement: those gains bring the water to target - PID_TUNE_REFINE_STEPS *
// PID_TUNE_REFINE_STEP and hold it; then PID_TUNE_REFINE_STEPS steps of
// PID_TUNE_REFINE_STEP up to the target are timed. After each step:
//  - overshoot above PID_TUNE_MAX_OVERSHOOT: P x0.75, integral 30 % slower;
//  - otherwise, settling slower than 4 theta: P x1.25 (at most 2x the computed P);
//  - a step that oscillates (crosses the setpoint 3+ times) drops D to 0.
// D follows P. The gains of the fastest step without overshoot are saved once
// the final step holds within +-PID_TUNE_BAND for PID_TUNE_STABLE_MS.
#define PID_TUNE_START_ABOVE_ROOM 10.0   // C: start condition
#define PID_TUNE_RISE_START  3.0
#define PID_TUNE_RISE_END    13.0
#define PID_TUNE_BAND        0.4
#define PID_TUNE_STABLE_MS   60000UL          // final hold
#define PID_TUNE_STEP_STABLE_MS 30000UL       // a refinement step is settled
#define PID_TUNE_STEP_MAX_MS (8UL * 60 * 1000) // a step that takes longer counts as slow
#define PID_TUNE_TIMEOUT_MS  (45UL * 60 * 1000)
#define PID_TUNE_REFINE_STEPS 3
#define PID_TUNE_REFINE_STEP  1.5
#define PID_TUNE_MAX_OVERSHOOT 0.2

static volatile int pidTuneState = PID_TUNE_IDLE;
static char pidTuneText[64] = "";
static double tuneStartTemp = 0, tuneTargetTemp = 0, savedP = 0, savedI = 0, savedD = 0;
static uint32_t tuneStartMs = 0, tuneRiseMs = 0, tuneStableSince = 0;
// Refinement (see above): step 0 = holding below the target, then 1..STEPS.
static int tuneStep = 0, tuneCrossings = 0, tuneSide = 0;
static double tuneSetpoint = 0, tuneStepMax = 0, tuneTheta = 0, tuneBaseP = 0;
static uint32_t tuneStepStart = 0;
static double bestP = 0, bestI = 0, bestD = 0, bestSettle = -1;

static bool pidTuneRunning(){
  return pidTuneState == PID_TUNE_HEATING || pidTuneState == PID_TUNE_SETTLING;
}

float pidTuneTarget(){
  float limit = floorf(currentBoilingPoint() + pidTuneOffset);
  return limit < MAX_HEATING_TARGET ? limit : MAX_HEATING_TARGET;
}

bool pidTuneReady(){
  float ambient = getAmbientTemp();
  return mokaStatus == 0 && !isnan(current) && current > -127.0 && !isnan(ambient) &&
         current <= ambient + PID_TUNE_START_ABOVE_ROOM;
}

bool pidTuneStart(){
  if(!pidTuneReady()) return false;
  savedP = p;
  savedI = i;
  savedD = d;
  tuneStartTemp = current;
  tuneTargetTemp = pidTuneTarget();
  setMokaStatus(4);
  target = tuneTargetTemp;
  estimateIntegral();
  tuneStartMs = millis();
  tuneRiseMs = 0;
  tuneStableSince = 0;
  pidTuneState = PID_TUNE_HEATING;
  snprintf(pidTuneText, sizeof(pidTuneText), "PID: heating, measuring");
  Serial.printf("PID calibration: from %.1f C to %.0f C\n", current, tuneTargetTemp);
  return true;
}

// Cancelled or failed: the previous gains come back.
static void pidTuneFail(const char *why){
  if(!pidTuneRunning()) return;
  p = savedP;
  i = savedI;
  d = savedD;
  estimateIntegral();
  pidTuneState = PID_TUNE_FAILED;
  snprintf(pidTuneText, sizeof(pidTuneText), "PID: %s", why);
  Serial.printf("PID calibration: %s\n", why);
}

void pidTuneCancel(){
  pidTuneFail("cancelled");
  if(mokaStatus == 4) setMokaStatus(0);
}

int pidTunePhase(){
  return pidTuneState;
}

const char * pidTuneMessage(){
  return pidTuneText;
}

void setPidTuneOffset(double degrees){
    prefsLock(); MokaPreferences.begin("my-app", false);
    pidTuneOffset = clampPidTuneOffset(degrees);
    MokaPreferences.putFloat("pidTuneOffset", pidTuneOffset);
    MokaPreferences.end(); prefsUnlock();
}

double getPidTuneOffset(){
  return pidTuneOffset;
}

// After each PID window in status 4.
static void pidTuneStartStep(int step, uint32_t now){
  tuneStep = step;
  tuneSetpoint = tuneTargetTemp - (PID_TUNE_REFINE_STEPS - step) * PID_TUNE_REFINE_STEP;
  tuneStepStart = now;
  tuneStepMax = current;
  tuneCrossings = 0;
  tuneSide = 0;
  tuneStableSince = 0;
}

// Scales P (and D with it) and the integral after a refinement step.
static void pidTuneScale(double pFactor, double integralFactor){
  double newP = p * pFactor;
  if(newP > 2 * tuneBaseP) newP = 2 * tuneBaseP;
  double applied = newP / p;
  p = newP;
  d *= applied;
  i *= applied * integralFactor;
  estimateIntegral();
}

// After each PID window in status 4.
static void pidTuneStep(){
  uint32_t now = millis();
  if(now - tuneStartMs > PID_TUNE_TIMEOUT_MS){
    pidTuneFail("timed out");
    setMokaStatus(0);
    return;
  }
  if(pidTuneState == PID_TUNE_HEATING){
    target = tuneTargetTemp;
    if(tuneRiseMs == 0 && current >= tuneStartTemp + PID_TUNE_RISE_START) tuneRiseMs = now;
    if(tuneRiseMs == 0 || current < tuneStartTemp + PID_TUNE_RISE_END) return;
    double rate = (PID_TUNE_RISE_END - PID_TUNE_RISE_START) / ((now - tuneRiseMs) / 1000.0);
    double theta = (tuneRiseMs - tuneStartMs) / 1000.0 - PID_TUNE_RISE_START / rate;
    if(theta < 1) theta = 1;
    double kc = 1.0 / (2 * rate * theta);
    double newP = timeWindow * kc;
    double ki = newP / (8 * theta);
    float ambient = getAmbientTemp();
    double delta = tuneTargetTemp - (isnan(ambient) ? 25 : ambient);
    double newI = delta > 1 ? ki / delta : 0;
    double newD = newP * (theta / 2) * 1e6;
    Serial.printf("PID calibration: R %.3f C/s, dead time %.1f s -> P %.2f, I %.5f, D %.0f\n",
                  rate, theta, newP, newI, newD);
    if(!(newP >= 1 && newP <= 500 && newI > 0 && newI <= 1)){
      pidTuneFail("implausible result");
      setMokaStatus(0);
      return;
    }
    p = newP;
    i = newI;
    d = newD;
    tuneTheta = theta;
    tuneBaseP = newP;
    bestSettle = -1;
    pidTuneState = PID_TUNE_SETTLING;
    pidTuneStartStep(0, now);
    target = tuneSetpoint;
    estimateIntegral();
    snprintf(pidTuneText, sizeof(pidTuneText), "PID: P %.1f I %.4f, to %.1f C", p, i, tuneSetpoint);
    return;
  }
  if(pidTuneState != PID_TUNE_SETTLING) return;

  target = tuneSetpoint;
  if(current > tuneStepMax) tuneStepMax = current;
  // Setpoint crossings (0.05 C hysteresis) tell an oscillating step.
  double error = current - tuneSetpoint;
  int side = error > 0.05 ? 1 : (error < -0.05 ? -1 : tuneSide);
  if(tuneSide != 0 && side != tuneSide) tuneCrossings++;
  tuneSide = side;

  bool finalStep = tuneStep == PID_TUNE_REFINE_STEPS;
  uint32_t needed = finalStep ? PID_TUNE_STABLE_MS : PID_TUNE_STEP_STABLE_MS;
  bool tooLong = tuneStep > 0 && now - tuneStepStart > PID_TUNE_STEP_MAX_MS;
  if(fabs(error) > PID_TUNE_BAND){
    tuneStableSince = 0;
    snprintf(pidTuneText, sizeof(pidTuneText), "PID: step %d/%d to %.1f C (%+.1f)",
             tuneStep, PID_TUNE_REFINE_STEPS, tuneSetpoint, error);
    if(!tooLong) return;
  }
  else{
    if(tuneStableSince == 0) tuneStableSince = now;
    uint32_t stable = now - tuneStableSince;
    snprintf(pidTuneText, sizeof(pidTuneText), "PID: step %d/%d stable %lu/%lu s",
             tuneStep, PID_TUNE_REFINE_STEPS, stable / 1000, needed / 1000);
    if(stable < needed && !tooLong) return;
  }

  if(tuneStep > 0){
    // Evaluate the step.
    double overshoot = tuneStepMax - tuneSetpoint;
    double settle = (tooLong ? now : tuneStableSince) - tuneStepStart;
    settle /= 1000.0;
    Serial.printf("PID calibration: step %d with P %.2f I %.5f D %.0f: overshoot %.2f C, settled in %.0f s%s, %d crossings\n",
                  tuneStep, p, i, d, overshoot, settle, tooLong ? " (too long)" : "", tuneCrossings);
    if(overshoot <= PID_TUNE_MAX_OVERSHOOT && !tooLong && (bestSettle < 0 || settle < bestSettle)){
      bestSettle = settle;
      bestP = p;
      bestI = i;
      bestD = d;
    }
    if(finalStep){
      if(bestSettle >= 0){
        p = bestP;
        i = bestI;
        d = bestD;
      }
      setPID(p, i, d);   // saved
      pidTuneState = PID_TUNE_DONE;
      snprintf(pidTuneText, sizeof(pidTuneText), "PID saved: P %.1f I %.4f D %.0f", p, i, d);
      Serial.printf("PID calibration: done, %s\n", pidTuneText);
      setMokaStatus(0);
      return;
    }
    if(tuneCrossings >= 3 && d != 0){
      d = 0;
      Serial.println("PID calibration: step oscillated, D = 0");
    }
    if(overshoot > PID_TUNE_MAX_OVERSHOOT) pidTuneScale(0.75, 1 / 1.3);
    else if(tooLong || settle > 4 * tuneTheta) pidTuneScale(1.25, 1);
  }
  pidTuneStartStep(tuneStep + 1, now);
  target = tuneSetpoint;
}

// Defined with the other manual power functions below.
static void manualPowerUpdate();

void process(void *param){
 // TickType_t startTime = xTaskGetTickCount();
  TickType_t xLastWakeTime = xTaskGetTickCount();
const TickType_t xFrequency = pdMS_TO_TICKS(250);
BaseType_t xWasDelayed;
  while(true){
//vTaskDelay((150) / portTICK_PERIOD_MS);
    //updateCurrentPressure();
     if(current > MAX_SAFE_TEMP){
      // Over-temperature: heater off and moka OFF, in every mode.
      connected=false;
      heaterDuty = 0;
      digitalWrite(pinHeater,getrelayStatus(LOW));
      if(mokaStatus != 0){
        Serial.printf("Over-temperature (%.1f C > %.0f C): switching off\n", current, MAX_SAFE_TEMP);
        setMokaStatus(0);
      }
    }
    else if(current <= -127.0 || mokaStatus == 0){
      connected=false;
      heaterDuty = 0;
      digitalWrite(pinHeater,getrelayStatus(LOW));
      // Manual power has no temperature control: losing the sensor stops it
      // for good instead of resuming when the reading comes back.
      if(mokaStatus == 3) setMokaStatus(0);
    }
    else if(mokaStatus == 3){
      // Manual power: fixed duty cycle, no PID, until the boiling point
      // (at the current pressure) + manualCutoffAboveBoiling is reached.
      connected=true;
      if(current >= currentBoilingPoint() + manualCutoffAboveBoiling){
        digitalWrite(pinHeater,getrelayStatus(LOW));
        setMokaStatus(0);
      }
      else{
        manualPowerUpdate();
      }
    }
    else{  
      connected=true;
        if(!running){
    pidUpdate();
    if(getMokaStatus() == 4) pidTuneStep();
    if(getMokaStatus() == 1 || getMokaStatus() == 2){
      double rate = 0, duty = 0;
      bool windowDone = waterEstimateStep(&rate, &duty);
      if(getMokaStatus() == 2) checkBrewEnd(windowDone, rate, duty);
    }
    if(getMokaStatus() == 2 && brewstart == false && (current) >= getCalibratedTemp()+ bloomOffset){
      brewstart = true;
      startTime = millis() - elapsedTime;
      //startTime = xTaskGetTickCount();
    }
    // Preheat target checked every cycle: its limit follows the boiling
    // point, which moves with the ambient pressure.
    if(getMokaStatus() == 1){
      double heatingTarget = calibrationHeating ? MAX_HEATING_TARGET : getTargetHeatingTemp();
      if(heatingTarget != target){
        target = heatingTarget;
        estimateIntegral();
      }
    }
    if(changePending){
      changePending = false;
      if(getMokaStatus() == 2){
        if(profileBrew){
          target = profileTarget;
        }
        else if(bloomfinished == false && bloomCurrentTargetTime > 0){
          target = getCalibratedTemp() + bloomOffset;
        }
        else{
          target = boil;
        }
        
      }
      estimateIntegral();
    }
  }

  }
    // Start the brew/bloom timer task once the brew begins; it deletes itself
    // as soon as the status leaves 2.
    if(getMokaStatus() == 2 && brewstart == true && brewTimerTask == NULL){
      // No I2C: runs on the UI core, away from the bus traffic.
      xTaskCreatePinnedToCore(brewTimer, "brewTimer", 2048, NULL, 4, &brewTimerTask, CORE_UI);
    }
xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
    //vTaskDelay( pdMS_TO_TICKS( 30 ));
}

}

// Updates brew and bloom times every 100 ms while extracting. Created by
// process() when the brew starts; runs in its own task because process()
// blocks for a whole PID time window in pidUpdate(). Ends itself as soon as
// the moka status is no longer 2.
void brewTimer(void *param){
  double aux;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(100);
  while(getMokaStatus() == 2){
    aux = millis() - startTime + elapsedTime;

    if(bloomTime >= bloomCurrentTargetTime || bloomfinished == true){
      bloomfinished = true;
      // A profile brew sets its own target (the bloom is part of its curve).
      if(!profileBrew) target = boil;
    }
    else{bloomTime = aux/1000;}
    brewTime = aux/1000;
    xTaskDelayUntil( &xLastWakeTime, xFrequency );
  }
  brewTimerTask = NULL;
  vTaskDelete(NULL);
}

void setBloomTime(double overrideTime){
    prefsLock(); MokaPreferences.begin("my-app", false);
    bloomCurrentTargetTime = overrideTime;
    bloomDefaultTargetTime= overrideTime;
    MokaPreferences.putFloat("bloomTime", bloomDefaultTargetTime);
    MokaPreferences.end(); prefsUnlock();
}
void setManualBloomTime(double overrideTime){
    bloomCurrentTargetTime = overrideTime;
}
void skipBloom(){
  if(brewstart == true){
    bloomCurrentTargetTime = bloomDefaultTargetTime;
    bloomfinished = true;
  }
    
}
void setManualCutoff(int degreesAboveBoiling){
    prefsLock(); MokaPreferences.begin("my-app", false);
    manualCutoffAboveBoiling = clampManualCutoff(degreesAboveBoiling);
    MokaPreferences.putInt("cutoffAbove", manualCutoffAboveBoiling);
    MokaPreferences.end(); prefsUnlock();
}

int getManualCutoff(){
  return manualCutoffAboveBoiling;
}

void setPreheatOffset(double degrees){
    prefsLock(); MokaPreferences.begin("my-app", false);
    preheatOffset = clampPreheatOffset(degrees);
    MokaPreferences.putFloat("preheatOffset", preheatOffset);
    MokaPreferences.end(); prefsUnlock();
}

double getPreheatOffset(){
  return preheatOffset;
}

void setOvershootMargin(double degrees){
    prefsLock(); MokaPreferences.begin("my-app", false);
    overshootMargin = clampOvershootMargin(degrees);
    MokaPreferences.putFloat("overshoot", overshootMargin);
    MokaPreferences.end(); prefsUnlock();
}

double getOvershootMargin(){
  return overshootMargin;
}

int getBrewStopReason(){
  return brewStopReason;
}

float getHeaterDuty(){
  return heaterDuty;
}

float getWaterGainRatio(){
  return waterGainRatio;
}

void setLowWaterRatio(double ratio){
    prefsLock(); MokaPreferences.begin("my-app", false);
    lowWaterRatio = clampLowWaterRatio(ratio);
    MokaPreferences.putFloat("lowWaterRatio", lowWaterRatio);
    MokaPreferences.end(); prefsUnlock();
}

double getLowWaterRatio(){
  return lowWaterRatio;
}

void calibrationHeatingOn(){
  setMokaStatus(1);
  calibrationHeating = true;
}

void calibrationHeatingOff(){
  calibrationHeating = false;
}

bool isCalibrationHeating(){
  return calibrationHeating;
}

void setBloomOffset(double overrideTemp){
    prefsLock(); MokaPreferences.begin("my-app", false);
    bloomOffset = overrideTemp;
    MokaPreferences.putFloat("bloomOffset", bloomOffset);
    MokaPreferences.end(); prefsUnlock();
}

double getBloomTargetTime(){

  return bloomCurrentTargetTime;
}

double getBloomTime(){
  if(bloomCurrentTargetTime > 0 ){
    return bloomTime;
  }
  else return -1;
}

double getBloomOffset(){
  return bloomOffset;
}

double getBrewTime(){
  return brewTime;
}

// Manual power mode (status 3): no PID, the relay is on for
// manualPowerPercent of every time window.
static volatile int manualPowerPercent = 100;

void setManualPower(int percent){
  if(percent < 0) percent = 0;
  if(percent > 100) percent = 100;
  manualPowerPercent = percent;
}

int getManualPower(){
  return manualPowerPercent;
}

// True when the temperature reading is valid (not disconnected / -127).
static bool temperatureValid(){
  return !isnan(current) && current > -127.0;
}

// Refuses to start without a valid temperature reading.
void manualPowerOn(int percent){
  if(!temperatureValid()) return;
  setManualPower(percent);
  setMokaStatus(3);
}

void setProfileTarget(float temp){
  profileTarget = temp > MAX_BOILING_TARGET ? MAX_BOILING_TARGET : temp;
  changePending = true;
}

// Refuses to start without a valid temperature reading. setMokaStatus()
// clears profileBrew, so it is set after the status.
void profileBrewOn(float firstTarget){
  if(!temperatureValid()) return;
  setMokaStatus(2);
  // No bloom phase of its own: the saved curve already holds it.
  bloomCurrentTargetTime = 0;
  setProfileTarget(firstTarget);
  profileBrew = true;
}

// One time window in manual mode: heater on for manualPowerPercent of it.
static void manualPowerUpdate(){
  running = true;
  heaterDuty = manualPowerPercent / 100.0;
  double onTime = timeWindow * manualPowerPercent / 100.0;
  if(onTime > 0){
    digitalWrite(pinHeater,getrelayStatus(HIGH));
    vTaskDelay( pdMS_TO_TICKS( onTime ));
  }
  digitalWrite(pinHeater,getrelayStatus(LOW));
  if(timeWindow - onTime > 0){
    vTaskDelay( pdMS_TO_TICKS( timeWindow - onTime ));
  }
  running = false;
}

 void pidUpdate(){
   running = true;
   temperature=current;
   myPID.run();
   double onTime = pidValue * gain;
   heaterDuty = onTime <= 0 ? 0 : (onTime >= timeWindow ? 1 : onTime / timeWindow);
    //if(pidValue>0){
     digitalWrite(pinHeater,getrelayStatus(HIGH));
     vTaskDelay( pdMS_TO_TICKS( pidValue * gain ));//}
     //if(pidValue<timeWindow){
      digitalWrite(pinHeater,getrelayStatus(LOW));
      vTaskDelay( pdMS_TO_TICKS( timeWindow-(pidValue * gain)));//}
   running = false;
 }

// Consecutive failed NTC readings after which the sensor is treated as
// disconnected (-127), which turns the heater off in process().
#define NTC_MAX_FAILED_READS 5
static int failedNtcReads = 0;

// A reading this far (C) from the current temperature is a sudden change:
// it is read again and only accepted if the new reading confirms it.
#define NTC_JUMP_CONFIRM 3.0

#define NTC_READ_OK           0
#define NTC_READ_FAILED       1   // I2C error, keep the previous temperature
#define NTC_READ_DISCONNECTED 2   // divider open or shorted

// One NTC reading: temperature (C) and the raw ADC value it came from.
static int readNtc(double *tempOut, double *ntcReadOut){
  // Read both channels first; if either failed (NAN), no temperature is
  // computed from bad data.
  double refRead = averageADSRead(3,5, 25, refVoltage);//ads.readADC_Differential_1_3();//
  double ntcRead = averageADSRead(0,8, 100, ntcValue);//ads.readADC_Differential_0_3();//
  if(isnan(refRead) || isnan(ntcRead)) return NTC_READ_FAILED;
  refVoltage = refRead;
  //refVoltage= refVoltage * 0.1875/1000;  f
  refVoltage = ads.toVoltage(1)*refVoltage;

double resistance;
 double kelvin;
 double vm = 0.0;
 double aux = 0;
vm = ads.toVoltage(1)*ntcRead;
  // NTC divider open or shorted (0 V or at the reference): no temperature can
  // be computed.
  if(vm <= 0 || vm >= refVoltage) return NTC_READ_DISCONNECTED;
resistance = (refVoltage - vm)*(resistorFixed/vm);
aux = shA + (shB * log(resistance)) + (shC * pow(log(resistance), 3));
kelvin =1.0/aux;
  *tempOut = kelvin - 273.15;
  *ntcReadOut = ntcRead;
  return NTC_READ_OK;
}

void updateTempNTC(){
  double rawTemp, ntcRead;
  int result = readNtc(&rawTemp, &ntcRead);
  if(result == NTC_READ_FAILED){
    if(++failedNtcReads >= NTC_MAX_FAILED_READS){
      current = -127.0f;
    }
    return;
  }
  failedNtcReads = 0;
  if(result == NTC_READ_DISCONNECTED){
    // Reported right away instead of NAN. ntcValue is reset so the outlier
    // filter does not compare against this reading.
    ntcValue = 0;
    current = -127.0f;
    return;
  }
  // Sudden change from a valid temperature: read again and accept it only if
  // the second reading agrees with the first; otherwise it was a glitch and
  // the current temperature is kept (the next cycle reads again).
  if(current > 0 && fabs(rawTemp - current) > NTC_JUMP_CONFIRM){
    double confirmTemp, confirmRead;
    if(readNtc(&confirmTemp, &confirmRead) != NTC_READ_OK ||
       fabs(confirmTemp - rawTemp) > NTC_JUMP_CONFIRM){
      Serial.printf("Temperature jump not confirmed: %.1f -> %.1f C, kept %.1f C\n",
                    current, rawTemp, current);
      return;
    }
    rawTemp = confirmTemp;
    ntcRead = confirmRead;
  }
  ntcValue = ntcRead;
  // Low-pass filter against the previous valid reading (current): changes
  // under 2 C are blended 75/25 to reduce noise, larger (confirmed) jumps pass
  // straight through. Without a valid previous reading (start-up, after a
  // disconnect) the raw value is used.
  double temp = rawTemp;
  if(current > 0 && fabs(rawTemp - current) < 2){
    temp = (rawTemp * 0.75) + (current * 0.25);
  }

  Serial.println(temp);
  if(temp>0){
      current=(temp);
  }
  else{current = -127.0f; }

}

float getCurrentTemp(){
    return current;
  }

int getMokaStatus(){
  return mokaStatus;
}
//0 OFF, 1 HEAT, 2 BOIL
void setMokaStatus(int status){
  changePending = true;
  mokaStatus = status;
  // Any status change ends a profile brew or a calibration (profileBrewOn
  // and calibrationHeatingOn set them again).
  profileBrew = false;
  calibrationHeating = false;
  targetReached = false;
  reachedTarget = NAN;
  brewStopReason = BREW_STOP_NONE;
  // Leaving the PID calibration any other way cancels it (OFF, another mode,
  // over-temperature); a new brew clears its last result.
  if(status != 4) pidTuneFail("cancelled");
  if(status != 0 && status != 4 && !pidTuneRunning()) pidTuneState = PID_TUNE_IDLE;
  if(status == 2) waterExtractionStart();
  else waterReset();
  if(status == 1 || status == 0 || status == 3){
    brewstart = false;
    bloomfinished = false;
    bloomTime = 0;
    brewTime = 0;
    // A manual bloom (long press EXTRACT: 300 s) only applies to that brew;
    // the next one uses the saved bloom time again.
    bloomCurrentTargetTime = bloomDefaultTargetTime;
  }
}
void brewMokaManual(double time){
  bloomCurrentTargetTime = time;
  setMokaStatus(2);
}
// Setpoint the PID is currently driving to (bloom or boiling target while
// extracting, heating target while preheating).
float getPidTarget(){
  return target;
}

float getTargetBoilingTemp(){
    return boil;
  }
// Limited to getHeatingLimit() when read, as the limit moves with the
// pressure (the saved value is kept).
float getTargetHeatingTemp(){
    float limit = getHeatingLimit();
    return heat < limit ? heat : limit;
  }
void setTargetBoilingTemp(float temp){
  boil = temp > MAX_BOILING_TARGET ? MAX_BOILING_TARGET : temp;
  changePending = true;
  }
// Whole degrees (floored): a profile's preheat temperature is a recorded
// reading such as 93.4 C.
void setTargetHeatingTemp(float temp){
  temp = floorf(temp);
  heat = temp > MAX_HEATING_TARGET ? MAX_HEATING_TARGET : temp;
  changePending = true;
  }

void getPID(double *pid){
  pid[0] = p; pid[1] = i; pid[2] = d;
}

void getResistorValues(double *data){
  //double aux[3];
  data[0] = resistorFixed;
  //data[1] = ntcRefResistor;
  //data[2] = ntcBeta;
  data[1] = shA;
  data[2] = shB;
  data[3] = shC;
  //return aux;
}

void setPID(double newP, double newI, double newD){
  prefsLock(); MokaPreferences.begin("my-app", false);
  p = newP; i = newI; d = newD;
  savePid(newP, newI, newD);
  // i is the per-degree factor: re-apply the scheduled Ki (saving any
  // setting mid-brew must not replace it with the raw factor).
  estimateIntegral();
  MokaPreferences.end(); prefsUnlock(); 
}
// Back to the firmware's default P, I and D: the stored ones are removed, so
// a later firmware with other defaults applies them too.
void resetPidDefaults(){
  prefsLock(); MokaPreferences.begin("my-app", false);
  MokaPreferences.remove("savedP");
  MokaPreferences.remove("kiPerDegree");
  MokaPreferences.remove("savedD");
  p = defaultp; i = defaulti; d = defaultd;
  estimateIntegral();
  MokaPreferences.end(); prefsUnlock();
}

void getDefaultPID(double *pid){
  pid[0] = defaultp; pid[1] = defaulti; pid[2] = defaultd;
}

void setResistorValues(double newR, double newA, double newB, double newC){
  prefsLock(); MokaPreferences.begin("my-app", false);
  // Applied right away (not only saved), so the temperature reading and the
  // settings page use the new values without a reboot.
  resistorFixed = newR;
  shA = newA; shB = newB; shC = newC;
  saveResistorValues(newR, newA, newB, newC);
  MokaPreferences.end(); prefsUnlock(); 
}

// Schedules Ki with the heat loss, which grows with the difference between
// the target and the room: Ki = i (the "I" setting, per degree) * delta.
// Called on every target change and when the PID settings are saved.
void estimateIntegral(){
  double delta = target - getAmbientTemp();
  if(delta < 0) delta = 0;
  myPID.setGains(p, i * delta, d);
}