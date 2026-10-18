#ifdef __cplusplus
extern "C" {
#endif


void init(void);
void pidUpdate(void);
void updateSensors(void *param);
void updateAmbientPressure(void *param);
void process(void *param);
void brewTimer(void *param);   
void updateTemp(void);
void updateTempNTC(void);
void averageTemp();
float getCurrentTemp();
int getMokaStatus();
//0 OFF, 1 HEAT, 2 BOIL
void setMokaStatus(int status);
void brewMokaManual(double time);
void setBloomTime(double overrideTime);
void setBloomOffset(double overrideTime);
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