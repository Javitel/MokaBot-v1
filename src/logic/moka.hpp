#ifdef __cplusplus
extern "C" {
#endif


void init(void);
void pidUpdate(void);
void updateSensors(void *param);
void updateAmbientPressure(void *param);
void process(void *param);   
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
void getResistorValues(double *pid);
double readPid();
void setPID(double newP, double newI, double newD);
void setResistorValues(double newR, double newN, double newB);
double getBloomTime();
double getBrewTime();
double getBloomOffset();
double getBloomTargetTime();
void setManualBloomTime(double overrideTime);
void skipBloom();
void adjustIntegral();
   #ifdef __cplusplus
      } // extern "C"
   #endif