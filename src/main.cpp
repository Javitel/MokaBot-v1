//*********************************************************************************************************/
//  WT32-SC01-PLUS template for platform.io
//  created by Frits Jan / productbakery on 11 oktober 2022
//
//
// When working with the squareline editor from lvgl, set the project in squareline to:
// - Arduino, with TFT_eSPI (which we cannot use, but will replace with LovyanGFX in this main.cpp file)
// - 480 x 320, 16 bit display
// 
// Export the template project AND export the UI Files
// You will get a project directory with two directories inside, 'ui' and 'libraries'
// From the libraries directory, copy the lv_conf.h to this projects /src/ directory (overwrite the old one)
// From the ui directory, copy all files to this projects src/ui/ directory (you can empty the ui directory first if needed)
// The ui.ino file can/should be deleted because this main.cpp files takes over.
//
//*********************************************************************************************************/

#define LGFX_USE_V1
#include "sdkconfig.h"
#include <LovyanGFX.hpp>
#include "logic/moka.hpp"
#include "logic/pressureSensor.hpp"
#include "logic/webserver.hpp"
#include "logic/pro.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEBUG_DECIMAL true

#define USE_DYNAMIC_PARAMETERS      false
bool LOAD_DEFAULT_CONFIG_DATA = true;

#define WT32_SC01
// #define SD_SUPPORTED // external SPI (SD or others)

#define SPI_HOST_ID SPI2_HOST

// Since SPI bus is shared, only CS PIN required
#define SD_CS   GPIO_NUM_33

// Portrait
#define TFT_WIDTH   320
#define TFT_HEIGHT  480

//#define SD_SUPPORTED

#define TFT_MOSI    GPIO_NUM_13 
#define TFT_MISO    -1  // Set this PIN for using shared SPI option
#define TFT_SCLK    GPIO_NUM_14
#define TFT_DC      GPIO_NUM_21 
#define TFT_CS      GPIO_NUM_15
#define TFT_RST     22

#define TOUCH_INT   GPIO_NUM_39
#define TOUCH_SDA   GPIO_NUM_18
#define TOUCH_SCL   GPIO_NUM_19

class LGFX : public lgfx::LGFX_Device
{
    // provide an instance that matches the type of panel you want to connect to.
    lgfx::Panel_ST7796 _panel_instance;

    // provide an instance that matches the type of bus to which the panel is connected.
    lgfx::Bus_SPI _bus_instance; // Instances of spi buses

    //Prepare an instance that matches the type of touchscreen. 
    lgfx::Touch_FT5x06  _touch_instance;

    lgfx::Light_PWM     _light_instance;

public:

  LGFX(void)
  {
    {
      // set up bus control.
      auto cfg = _bus_instance.config(); // gets the structure for bus settings.

      // SPI bus settings
      cfg.spi_host = SPI2_HOST; // Select SPI to use ESP32-S2,C3: SPI2_HOST or SPI3_HOST / ESP32: VSPI_HOST or HSPI_HOST

      //* Due to the ESP-IDF upgrade, the description of VSPI_HOST , HSPI_HOST will be deprecated, so if you get an error, use SPI2_HOST , SPI3_HOST instead.
      cfg.spi_mode = 0;          // Set SPI communication mode (0-3) 
      cfg.freq_write = 40000000;//80000000; // SPI clock on transmission (up to 80MHz, rounded to 80MHz divided by integer)
      cfg.freq_read = 8000000;//16000000;  // SPI clock on reception
      cfg.spi_3wire = true;      // Set true when receiving on the MOSI pin
      cfg.use_lock = false;//true;       // set true if transaction lock is used
 
      //  * With the ESP-IDF version upgrade, SPI_DMA_CH_AUTO (automatic setting) of DMA channels is recommended. 
      cfg.dma_channel = SPI_DMA_CH_AUTO; // Set DMA channel to be used (0=DMA not used / 1=1ch / 2=ch / SPI_DMA_CH_AUTO=Auto setting)

      cfg.pin_sclk = TFT_SCLK; // Set SCLK pin number for SPI
      cfg.pin_mosi = TFT_MOSI; // Set SPI MOSI pin number

      // When using the SPI bus, which is common to the SD card, be sure to set MISO without omitting it.
      cfg.pin_miso = TFT_MISO; // Set THE MSO pin number of spi (-1 = disable)
      cfg.pin_dc = TFT_DC;    // Set THE D/C pin number of SPI (-1 = disable)

      _bus_instance.config(cfg);              // reflects the setting value on the bus.
      _panel_instance.setBus(&_bus_instance); // Set the bus to the panel.
    }

    {
      // set the display panel control.
      auto cfg = _panel_instance.config(); // gets the structure for display panel settings.

      cfg.pin_cs = TFT_CS;    // Pin number to which CS is connected (-1 = disable)
      cfg.pin_rst = TFT_RST;  // Pin number to which RST is connected (-1 = disable)
      cfg.pin_busy = -1;      // Pin number to which BUSY is connected (-1 = disable)

      // the following setting values are set to a general initial value for each panel,
      cfg.panel_width = TFT_WIDTH;    // actual visible width
      cfg.panel_height = TFT_HEIGHT;   // actually visible height
      cfg.offset_x = 0;         // Panel X-direction offset amount
      cfg.offset_y = 0;         // Panel Y offset amount
      cfg.offset_rotation = 0;  // offset of rotational values from 0 to 7 (4 to 7 upside down)
      cfg.dummy_read_pixel = 8; // number of bits in dummy leads before pixel read
      cfg.dummy_read_bits = 1;  // number of bits in dummy leads before reading non-pixel data
      cfg.readable = false;      // set to true if data can be read
      cfg.invert = false;       // set to true if the light and dark of the panel is reversed
      cfg.rgb_order = false;    // set to true if the red and blue of the panel are swapped
      cfg.dlen_16bit = false;   // Set to true for panels that transmit data lengths in 16-bit increments in 16-bit parallel or SPI
      cfg.bus_shared = false;    // Set to true when sharing the bus with sd card (bus control is performed with drawJpgFile, etc.)

      // The following should only be set if the display is misalized by a driver with a variable number of pixels, such as st7735 or ILI9163.
      // cfg.memory_width = 240; //Maximum width supported by driver ICs
      // cfg.memory_height = 320; //Maximum height supported by driver ICs

      _panel_instance.config(cfg);
    }

    {
      auto cfg = _light_instance.config();    

      cfg.pin_bl = 23;//45;              
      cfg.invert = false;           
      cfg.freq   = 44100;           
      cfg.pwm_channel = 7;          

      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);  
    }

    { 
      auto cfg = _touch_instance.config();

      cfg.x_min      = 0;
      cfg.x_max      = 319;
      cfg.y_min      = 0;  
      cfg.y_max      = 479;
      cfg.pin_int    = TOUCH_INT;  
      cfg.bus_shared = true; 
      cfg.offset_rotation = 0;

      cfg.i2c_port = 0;//I2C_NUM_1;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda  = TOUCH_SDA;   
      cfg.pin_scl  = TOUCH_SCL;   
      cfg.freq = 400000;  

      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);  
    }

    setPanel(&_panel_instance); // Set the panel to be used.
  }
};

#include <lvgl.h>
#include "ui/ui.h" // this is the ui generated with lvgl / squareline editor

LGFX tft;

#define screenWidth 480
#define screenHeight 320

// lv debugging can be set in lv_conf.h
#if LV_USE_LOG != 0
/* Serial debugging */
void my_print(const char * buf)
{
    Serial.printf(buf);
    Serial.flush();
}
#endif

// create buffer for display
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10];

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((lgfx::rgb565_t *)&color_p->full, w * h);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

/*Read the touchpad*/
void my_touch_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    uint16_t touchX, touchY;
    bool touched = tft.getTouch(&touchX, &touchY);
    if (!touched) { data->state = LV_INDEV_STATE_REL; }
    else {
      data->state = LV_INDEV_STATE_PR;
      data->point.x = touchX;
      data->point.y = touchY;

      #if DEBUG_TOUCH !=0
      Serial.print( "Data x " ); Serial.println( touchX );
      Serial.print( "Data y " ); Serial.println( touchY );
      #endif
    }
}

//************************************************************************************
//  SETUP AND LOOP
//************************************************************************************


int frame = 0;
float currentPressure;
float currentTemp;
float aux;
String text;

TaskHandle_t PIDTask = NULL;  ///  Task handler fot the LED Task
TaskHandle_t UITask = NULL;  ///  Task handler fot the LED Task
TaskHandle_t WebserviceTask = NULL;  ///  Task handler fot the LED Task
TaskHandle_t SensorTask = NULL;  ///  Task handler fot the LED Task
TaskHandle_t LabelsTasks = NULL;  ///  Task handler fot the LED Task
TaskHandle_t PressureTask = NULL;  ///  Task handler fot the LED Task
//TaskHandle_t ProTask = NULL;  ///  Task handler fot the LED Task

// static lv_img_dsc_t my_img_dsc = {
//     .header.always_zero = 0,
//     .header.w = 80,
//     .header.h = 60,
//     .data_size = 80 * 60 * LV_COLOR_DEPTH / 8,
//     .header.cf = LV_IMG_CF_TRUE_COLOR,          /*Set the color format*/
//     .data = my_img_data,
// };

String prepareLabel(float value, int precision, String label){
  return String(value, precision)+label;
}

String htmlTemp() {

    return String(currentTemp);
}

void refreshUI(void *param){
  /*Initialize the input device driver*/

       TickType_t xLastWakeTime;
const TickType_t xFrequency = pdMS_TO_TICKS(20);
BaseType_t xWasDelayed;
xLastWakeTime = xTaskGetTickCount ();
  while(true){
    lv_timer_handler(); 
    xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );  }
}


void calculateLabels(void *param){
  TickType_t xLastWakeTime;
const TickType_t xFrequency = pdMS_TO_TICKS(20);
BaseType_t xWasDelayed;
  //showScaleButton(0);
  while(true){
    vTaskDelay( pdMS_TO_TICKS( 15 ));
  setCurrentIP(getIp());
    currentPressure = getCurrentPressure();
    currentTemp = getCurrentTemp();
    //Set Labels
    if(currentTemp <= -50.0 || !(currentTemp==currentTemp)){
      setCurrentTempLabel(String("DISC.").c_str());  
    }
    else{
      if(DEBUG_DECIMAL){
        setCurrentTempLabel(prepareLabel(currentTemp,1,"c").c_str()); 
      }
      else{
        setCurrentTempLabel(prepareLabel(currentTemp,0,"c").c_str()); 
      }
      //setCurrentTempLabel(prepareLabel(currentTemp,1,"c").c_str()); 
    }
    setCurrentPressure(prepareLabel(currentPressure/100000,3,"bar").c_str());
      if(getMokaStatus() ==1){
        if(DEBUG_DECIMAL){
          setTargetTempLabel(prepareLabel(getTargetHeatingTemp(),1,"c").c_str());
        }
        else{
          setTargetTempLabel(prepareLabel(getTargetHeatingTemp(),0,"c").c_str());
        }
    //setTargetTempLabel(prepareLabel(getTargetHeatingTemp(),1,"c").c_str());
    aux = getBoilingTemp(currentPressure/100000);
    if(DEBUG_DECIMAL){
      text = "boils at: "+ String(aux,1)+"c";
    }
    else{
      text = "boils at: "+ String(aux,0)+"c";
    }
    //aux = getBoilingTemp(currentPressure/100000);
    //text = "boils at: "+ String(aux,1) +"c";
    setExtraInfo(text.c_str());
  }
  if(getMokaStatus() ==2){ 
    if(DEBUG_DECIMAL){
      setTargetTempLabel(prepareLabel(getTargetBoilingTemp(),1,"c").c_str());
    }
    else{
      setTargetTempLabel(prepareLabel(getTargetBoilingTemp(),0,"c").c_str());
    }
    //setTargetTempLabel(prepareLabel(getTargetBoilingTemp(),1,"c").c_str());
    aux = getPressureByTemp(getTargetBoilingTemp(),getCalibratedPressure());
    text = "aprox Press.: "+ (String)aux+"bar";
    setExtraInfo(text.c_str());
    //temp brew time and bloom time
    
    if(getBloomTime() == -1){
      text = "OFF";
    }
    else{
      if(DEBUG_DECIMAL){
        text = "Bloom: "+String(getBloomTime(),1) + "s";
      }
      else{
        text = "Bloom: "+String(getBloomTime(),0) + "s";
      }
      //text = "Bloom: "+String(getBloomTime()) + "s";
    }
    setProWeigth(text.c_str());
    if(DEBUG_DECIMAL){
      text = "Brew: "+String(getBrewTime(),1) + "s";
    }
    else{
      text = "Brew: "+String(getBrewTime(),0) + "s";
    }
    //text = "Brew: "+String(getBrewTime()) + "s";
    setProTemp(text.c_str());
  }
  else{
    setProWeigth(String("").c_str());
    setProTemp(String("").c_str());
  }
  if(getMokaStatus() ==0 ){
    setTargetTempLabel(String("OFF").c_str());
    text = "";
    setExtraInfo(text.c_str());
  }
  xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
  // if(isBaseConnected() == 1){
  //   showScaleButton(1);
  //   text = String(getWeight()) + "g";
  //   setProWeigth(text.c_str());
  //   text = String(getBaseTemp()) + "c";
  //   setProTemp(text.c_str());
  // }
  // else{
  //   showScaleButton(0);
  //   setProWeigth(String("").c_str());
  //   setProTemp(String("").c_str());
  // }
}
}

void setup() {
  Serial.begin(115200);
   tft.begin();
  tft.setRotation(3);
  tft.setBrightness(100);
      lv_init();
          #if LV_USE_LOG != 0
    lv_log_register_print_cb( my_print ); /* register print function for debugging */
  #endif
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
 

  unsigned status;
  initSensor();
  init();
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);
  //lv_obj_t * img_src = lv_img_create(lv_scr_act()); /*Crate an image object*/
    //lv_img_set_src(img_src, &my_img_data);  /*Set the created file as image (a red fl  ower)*/
    //lv_obj_set_pos(img_src, 0, 0);  
  // start the UI
    ui_init();
  ui_initBoot();
       lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);
  //showSplashScreen(1);

  //loop();//Load Screen
  currentPressure = getCurrentPressure();
      xTaskCreatePinnedToCore(process, "process",2048, NULL, 7, &PIDTask,1);     
      xTaskCreatePinnedToCore(webserverService, "webserver",4096, NULL, 3, &WebserviceTask,1);
      xTaskCreatePinnedToCore(updateSensors, "sensors",4096, NULL, 8, &SensorTask,0);
      //xTaskCreate(updateSensors, "sensors",4098, NULL, 8, &SensorTask);
      xTaskCreatePinnedToCore(updateAmbientPressure, "pressure",2048, NULL, 4, &PressureTask,0);
      //xTaskCreate(updateAmbientPressure, "pressure",2048, NULL, 4, &PressureTask);
      //xTaskCreate(refreshUI, "UI",4096, NULL, 9, &UITask);
      xTaskCreatePinnedToCore(refreshUI, "UI",4096, NULL, 9, &UITask,0);
      xTaskCreatePinnedToCore(calculateLabels, "labels",4096, NULL, 8, &LabelsTasks,1);
      //xTaskCreate(calculateLabels, "labels",4096, NULL, 8, &LabelsTasks);
      //xTaskCreate(proRoutine, "pro",4096, NULL, 3, &ProTask);
      delay(1000);
      showSplashScreen(0);
  ui_init();
      //lv_obj_invalidate(lv_scr_act()); // Mark the active screen for full redraw
    //lv_task_handler();
      //lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);
  //espIp = String(initNet());
  //Serial.println(espIp);
}

void loop() {
  //Serial.println(bmp.readPressure());
  delay(1000);
}
