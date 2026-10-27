#include "pressureSensor.hpp"
#include <Adafruit_BMP280.h>
#include <Preferences.h>
#include "logic/prefsLock.hpp"
#include "logic/moka.hpp"
#include "devices/boardPins.hpp"
#include <Wire.h>

#define BMP280_ADDRESS     (0x76)  // SDO to GND
#define BMP280_ADDRESS_ALT (0x77)  // SDO to VCC

// Set at startup: when no BMP280 answers, its reads are skipped instead of
// NACKing on the shared I2C bus every second.
static bool bmpAvailable = false;

Adafruit_BMP280 bmp;

// Chip IDs read through the BMP280 driver: BMP280 (0x58), BMP280 engineering
// samples / clones (0x56, 0x57) and BME280 (0x60), whose temperature and
// pressure registers and calibration match the BMP280.
static const uint8_t BMP_CHIP_IDS[] = { 0x58, 0x56, 0x57, 0x60 };

// Diagnostic: lists every address that ACKs on the sensor I2C bus.
// Short timeout while scanning: on a stuck bus every probe times out, and
// 112 probes at the normal 200 ms took ~22 s of boot time.
#define I2C_SCAN_TIMEOUT_MS 10

static void scanI2cBus(){
  Serial.print("I2C devices found:");
  Wire.setTimeOut(I2C_SCAN_TIMEOUT_MS);
  int count = 0;
  int timeouts = 0;
  for(uint8_t address = 0x08; address < 0x78; address++){
    Wire.beginTransmission(address);
    uint8_t result = Wire.endTransmission();
    if(result == 0){
      Serial.printf(" 0x%02X", address);
      count++;
    }
    else if(result == 5){ // 5 = timeout
      timeouts++;
    }
  }
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  Serial.println(count ? "" : " none");
  // Timeouts (rather than NACKs) mean the bus itself is not working:
  // SDA/SCL held low, missing pull-ups or wiring.
  if(timeouts > 0){
    Serial.printf("I2C bus: %d probe timeouts - check SDA/SCL wiring and pull-up resistors\n", timeouts);
  }
}

// Tries both addresses and every accepted chip ID; logs what was found.
static bool detectBmp(){
  const uint8_t addresses[] = { BMP280_ADDRESS, BMP280_ADDRESS_ALT };
  for(uint8_t address : addresses){
    for(uint8_t chipId : BMP_CHIP_IDS){
      if(bmp.begin(address, chipId)){
        Serial.printf("Pressure sensor found at 0x%02X, chip ID 0x%02X%s\n",
                      address, chipId, chipId == 0x60 ? " (BME280)" : "");
        return true;
      }
    }
    // Something answered but with an unknown ID: report it for diagnosis.
    if(bmp.sensorID() != 0){
      Serial.printf("Device at 0x%02X has unsupported chip ID 0x%02X\n", address, bmp.sensorID());
    }
  }
  return false;
}

Preferences preferences;

float boilingPoint =100.0;
float boilingPointPressure = 1.0;

float readPressure = 0.0;

void initSensor(){
      prefsLock(); preferences.begin("my-app", false); 

  boilingPoint = preferences.getFloat("boilingPoint", boilingPoint);
  boilingPointPressure = preferences.getFloat("boilPointPres", boilingPointPressure);
  setTargetBoilingTemp(preferences.getFloat("boilingTemp", getTargetBoilingTemp()));
  setTargetHeatingTemp(preferences.getFloat("heatingTemp", getTargetHeatingTemp()));
  preferences.end(); prefsUnlock();
  // Shared with the ADS1115 (started later in init()). On boards whose
  // display driver already started Wire on these pins, this is a no-op.
  Wire.begin(PIN_SENSOR_SDA, PIN_SENSOR_SCL);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  bmpAvailable = detectBmp();
  if(!bmpAvailable){
    Serial.println("BMP280 not found at 0x76/0x77: pressure and ambient temperature disabled");
    scanI2cBus();
    return;
  }
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X16,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_63); /* Standby time. */
                  delay(100);
}

float getCurrentPressure(){
    return readPressure;//bmp.readPressure();
}

// ---- Checked BMP280 reads ---------------------------------------------------
// A read is retried when it differs more than BMP_MAX_DEVIATION from the
// previous value (likely a corrupted transfer). It is accepted when it is
// within that deviation of the previous value, or when the next read confirms
// it (within the deviation of each other: a real change). NAN when no read is
// accepted after BMP_READ_ATTEMPTS, so the caller keeps its previous value.
#define BMP_MAX_DEVIATION  0.01  // 1%
#define BMP_READ_ATTEMPTS  3

static bool withinDeviation(float value, float reference){
    return fabsf(value - reference) <= fabsf(reference) * BMP_MAX_DEVIATION;
}

static float readBmpChecked(float (*read)(), bool hasPrevious, float previous,
                            float minValid, float maxValid){
    float lastAttempt = NAN;
    for(int attempt = 0; attempt < BMP_READ_ATTEMPTS; attempt++){
        float value = NAN;
        try{
          value = read();
        } catch (...){}
        if(isnan(value) || value < minValid || value > maxValid) continue;
        if(!hasPrevious || withinDeviation(value, previous)) return value;
        if(!isnan(lastAttempt) && withinDeviation(value, lastAttempt)) return value;
        lastAttempt = value;
    }
    return NAN;
}

// Last valid ambient temperature, refreshed by updateAmbientTemp() in the
// pressure task. Callers (estimateIntegral in the PID task) only read this
// variable: no I2C, library or error-logging code runs on their stack.
static volatile float lastAmbientTemp = 20.0;
static bool ambientTempValid = false;

void updateAmbientTemp(){
    if(!bmpAvailable) return;
    float temp = readBmpChecked([]{ return bmp.readTemperature(); },
                                ambientTempValid, lastAmbientTemp, -40.0, 85.0);
    if(!isnan(temp)){
      lastAmbientTemp = temp;
      ambientTempValid = true;
    }
}

float getAmbientTemp(){
    return lastAmbientTemp;
}

// Keeps the last valid pressure when a read fails or is not confirmed.
// BMP280 range: 300-1100 hPa.
void updateCurrentPressure(){
  if(!bmpAvailable) return;
  float pressure = readBmpChecked([]{ return bmp.readPressure(); },
                                  readPressure > 0, readPressure, 30000.0, 110000.0);
  if(!isnan(pressure)){
    readPressure = pressure;
  }
}
void overrideBoiling(double input){
    prefsLock(); preferences.begin("my-app", false);
    boilingPoint = input;
     preferences.putFloat("boilingPoint", boilingPoint);
  boilingPointPressure = readPressure/100000;
  preferences.putFloat("boilPointPres", boilingPointPressure);
  preferences.end(); prefsUnlock();
}
// Calibration heats towards 110 C (past the preheat limit) and keeps the last
// CALIBRATION_WINDOW_SAMPLES readings: once they are within
// CALIBRATION_STABLE_RANGE the water is boiling and the highest one is saved.
#define CALIBRATION_SAMPLE_MS      100
#define CALIBRATION_WINDOW_SAMPLES 30     // 3 s
#define CALIBRATION_STABLE_RANGE   0.5
#define CALIBRATION_TIMEOUT_MS     (60UL * 1000)       // 1 min
// Calibration starts this many degrees below the theoretical boiling point at
// the altitude (from the ambient pressure); a plateau below that is not
// boiling yet and heating continues. Without a valid pressure reading,
// CALIBRATION_MIN_VALID is used instead.
#define CALIBRATION_START_BELOW 5.0
#define CALIBRATION_MIN_VALID  85.0
// Above this the result is rejected (sensor fault).
#define CALIBRATION_MAX_VALID  105.0
// How long the calibration message stays on the screen's info line.
#define CALIBRATION_MESSAGE_MS 4000

static volatile bool calibrating = false;
// Start temperature of the running calibration (see calibrationStartTemp()).
static float calibrationMinTemp = CALIBRATION_MIN_VALID;

// Theoretical boiling point of water (C) at an ambient pressure (bar), from
// physics only (Clausius-Clapeyron from 100 C at 1 atm), so a wrong
// calibration does not affect it. 0.578 bar (~4500 m) gives 85 C.
static float theoreticalBoilingTemp(float bar){
    return 1.0 / (1.0 / 373.15 - (8.3145 / 40700.0) * log(bar / 1.01325)) - 273.15;
}

// Lowest temperature calibration starts at: theoretical boiling point minus
// CALIBRATION_START_BELOW (95 C at sea level, ~87.5 C at 2240 m).
static float calibrationStartTemp(){
    float bar = getCurrentPressure() / 100000;
    // No BMP280 or an implausible reading (outside ~-800 to 5500 m).
    if(isnan(bar) || bar < 0.5 || bar > 1.1) return CALIBRATION_MIN_VALID;
    return theoreticalBoilingTemp(bar) - CALIBRATION_START_BELOW;
}

// Short message about the last calibration for the screen (see
// calibrationMessage()); empty when there is nothing to show. The temperature
// is kept apart, in Celsius, so the screen formats it in its selected unit.
static char calibrationText[32] = "";
static volatile float calibrationTemp = NAN;
static volatile uint32_t calibrationTextUntil = 0;
static volatile bool calibrationTextForever = false;   // while calibrating

static void setCalibrationMessage(const char *text, bool untilReplaced, float temp = NAN){
    strlcpy(calibrationText, text, sizeof(calibrationText));
    calibrationTemp = temp;
    calibrationTextForever = untilReplaced;
    calibrationTextUntil = millis() + CALIBRATION_MESSAGE_MS;
}

const char * calibrationMessage(float *temp){
    *temp = NAN;
    if(calibrationText[0] == '\0') return "";
    if(!calibrationTextForever && (int32_t)(millis() - calibrationTextUntil) > 0) return "";
    *temp = calibrationTemp;
    return calibrationText;
}

static bool validReading(float temp){
    return !isnan(temp) && temp > -50.0;
}

// Waits for the temperature to stop rising (see CALIBRATION_STABLE_RANGE)
// and stores the highest reading of that window, with the pressure read at
// that moment, as the boiling calibration point. Then preheat goes back to
// its normal (limited) target.
static void calibrateBoilingTask(void *param){
    float window[CALIBRATION_WINDOW_SAMPLES];
    int filled = 0, next = 0;
    uint32_t start = millis();
    float result = NAN;
    const char *failure = NULL;
    while(true){
        vTaskDelay(pdMS_TO_TICKS(CALIBRATION_SAMPLE_MS));
        // OFF, EXTRACT, over-temperature...: any status change cancels it.
        if(!isCalibrationHeating()){ failure = "Calibration cancelled"; break; }
        if(millis() - start > CALIBRATION_TIMEOUT_MS){ failure = "Calibration timed out"; break; }
        float temp = getCurrentTemp();
        if(!validReading(temp)){
            // Lost reading: start the window over.
            filled = 0;
            next = 0;
            continue;
        }
        if(temp > CALIBRATION_MAX_VALID){ result = temp; break; }
        window[next] = temp;
        next = (next + 1) % CALIBRATION_WINDOW_SAMPLES;
        if(filled < CALIBRATION_WINDOW_SAMPLES) filled++;
        if(filled < CALIBRATION_WINDOW_SAMPLES) continue;
        float low = window[0], high = window[0];
        for(int k = 1; k < CALIBRATION_WINDOW_SAMPLES; k++){
            if(window[k] < low) low = window[k];
            if(window[k] > high) high = window[k];
        }
        if(high - low < CALIBRATION_STABLE_RANGE && high >= calibrationMinTemp){
            result = high;
            break;
        }
    }
    calibrationHeatingOff();

    if(failure){
        setCalibrationMessage(failure, false);
    }
    else if(result > CALIBRATION_MAX_VALID){
        // Implausible boiling point: keep the previous calibration.
        setCalibrationMessage("Rejected: ", false, result);
        Serial.printf("Boiling calibration rejected: %.1f C above %.0f C\n", result, CALIBRATION_MAX_VALID);
    }
    else{
        setCalibrationMessage("Boiling: ", false, result);
        prefsLock(); preferences.begin("my-app", false);
        boilingPoint = result;
        preferences.putFloat("boilingPoint", boilingPoint);
        boilingPointPressure = readPressure/100000;
        preferences.putFloat("boilPointPres", boilingPointPressure);
        preferences.end(); prefsUnlock();
    }
    calibrating = false;
    vTaskDelete(NULL);
}

// Non-blocking: heats and waits in its own task so the caller (an LVGL
// event) does not freeze the UI. Ignored while a calibration is running.
// Starts from OFF or preheat (switching to preheat), from
// CALIBRATION_START_BELOW below the theoretical boiling point.
void calibrateBoiling(){
    if(calibrating) return;
    int status = getMokaStatus();
    if(status != 0 && status != 1){
        setCalibrationMessage("Calibrate from OFF/preheat", false);
        return;
    }
    float temp = getCurrentTemp();
    if(!validReading(temp)){
        setCalibrationMessage("Calibration: no reading", false);
        return;
    }
    // Only near boiling, so the 1 min timeout is enough to reach it.
    float startTemp = calibrationStartTemp();
    if(temp < startTemp){
        setCalibrationMessage("Calibrate at >=", false, startTemp);
        return;
    }
    calibrationMinTemp = startTemp;
    calibrating = true;
    calibrationHeatingOn();
    setCalibrationMessage("Calibrating...", true);
    if(xTaskCreate(calibrateBoilingTask, "calibrate", 3072, NULL, 5, NULL) != pdPASS){
        calibrationHeatingOff();
        calibrating = false;
        setCalibrationMessage("Calibration failed", false);
    }
}
float getCalibratedTemp(){
    return boilingPoint;
}
double getCalibratedPressure(){
    return boilingPointPressure;
}

void saveTargetHeatingTemp(float temp){
    prefsLock(); preferences.begin("my-app", false);
    preferences.putFloat("heatingTemp", temp);
    preferences.end(); prefsUnlock();
}

void saveTargetBoilingTemp(float temp){
    prefsLock(); preferences.begin("my-app", false);
    preferences.putFloat("boilingTemp", temp);
    preferences.end(); prefsUnlock();
}

float getBoilingTemp(float pressure){
    double atm = pressure;// * 0.986923;
      //double aux = log(getCalibratedPressure()/atm);
    double aux = log(atm/getCalibratedPressure());
    aux = (8.3145/40700) * aux;
    aux = (1.0/(getCalibratedTemp()+273.15))-aux;
    aux = (1.0/aux);

    return (float)aux-273.15;
  
}

float getPressureByTemp(float temp, float pressure){
    if(temp < getBoilingTemp(pressure/100000)){
      return getCurrentPressure()/100000;
    }
    else{
      // Clausius-Clapeyron from the calibration point. The calibrated pressure
      // is already in bar, so the result is in bar too (no atm conversion),
      // and it matches the ambient pressure exactly at the boiling point.
      float factor = (40700.0/8.3145)*((1/(getCalibratedTemp()+273.15))-(1/(temp+273.15)));
      return getCalibratedPressure()*exp(factor);
    }
  }