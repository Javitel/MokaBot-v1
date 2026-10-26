#if defined(BOARD_WT32_SC01)

#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include <Wire.h>
#include "wt32-sc01.hpp"
#include "boardPins.hpp"

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

// Touch controller (FT6336, FT5x06 family) on the I2C bus GPIO 18/19, shared
// with the BMP280 and ADS1115 (PIN_SENSOR_SDA/SCL). It is read through Arduino
// Wire like the sensors: a single I2C driver for the whole bus. (LovyanGFX's
// own touch driver programmed the same I2C hardware separately, so its
// transfers and Wire's collided.)
#define TOUCH_ADDR      0x38
#define TOUCH_REG_DATA  0x02   // TD_STATUS, then P1 XH, XL, YH, YL

class LGFX : public lgfx::LGFX_Device
{
    // provide an instance that matches the type of panel you want to connect to.
    lgfx::Panel_ST7796 _panel_instance;

    // provide an instance that matches the type of bus to which the panel is connected.
    lgfx::Bus_SPI _bus_instance; // Instances of spi buses

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

    setPanel(&_panel_instance); // Set the panel to be used.
  }
};

static LGFX tft;


// lv debugging can be set in lv_conf.h
#if LV_USE_LOG != 0
/* Serial debugging */
static void my_print(const char * buf)
{
    Serial.printf(buf);
    Serial.flush();
}
#endif

// create buffer for display
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10];

/* Display flushing */
static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((lgfx::rgb565_t *)&color_p->full, w * h);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

/*Read the touchpad*/
// How often LVGL reads the touch controller on this board.
#define TOUCH_READ_PERIOD_MS 75

// Reads the first touch point in native panel coordinates (320x480 portrait).
// False when nothing touches the screen or the read fails.
static bool readTouchRaw(uint16_t *x, uint16_t *y) {
    uint8_t data[5];
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(TOUCH_REG_DATA);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)sizeof(data)) != sizeof(data)) return false;
    Wire.readBytes(data, sizeof(data));
    uint8_t points = data[0] & 0x0F;
    if (points == 0 || points > 2) return false;
    *x = ((data[1] & 0x0F) << 8) | data[2];
    *y = ((data[3] & 0x0F) << 8) | data[4];
    return *x < TFT_WIDTH && *y < TFT_HEIGHT;
}

static void my_touch_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    uint16_t rawX, rawY;
    if (!readTouchRaw(&rawX, &rawY)) { data->state = LV_INDEV_STATE_REL; }
    else {
      // Same conversion LovyanGFX applied for setRotation(3): swap the axes,
      // then mirror the new x.
      uint16_t touchX = (TFT_HEIGHT - 1) - rawY;
      uint16_t touchY = rawX;
      data->state = LV_INDEV_STATE_PR;
      data->point.x = touchX;
      data->point.y = touchY;

      #if DEBUG_TOUCH !=0
      Serial.print( "Data x " ); Serial.println( touchX );
      Serial.print( "Data y " ); Serial.println( touchY );
      #endif
    }
}

void displayInit(){
  // Shared I2C bus (touch + sensors), started before anything uses it;
  // initSensor()'s Wire.begin() then finds it running.
  Wire.begin(PIN_SENSOR_SDA, PIN_SENSOR_SCL);
  Wire.setTimeOut(I2C_TIMEOUT_MS);

  tft.begin();
  tft.setRotation(3);
  // Backlight 0-255 (26 = 10%).
  tft.setBrightness(55);

  lv_init();
  #if LV_USE_LOG != 0
    lv_log_register_print_cb( my_print ); /* register print function for debugging */
  #endif

  // The draw buffer must be initialised before the driver that uses it.
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);
  // Touch read period for this board only (LV_INDEV_DEF_READ_PERIOD is shared).
  lv_timer_set_period(indev_drv.read_timer, TOUCH_READ_PERIOD_MS);
}

#endif // BOARD_WT32_SC01
