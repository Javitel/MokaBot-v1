#if defined(BOARD_WAVESHARE_S3_TOUCH_LCD_35B)

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "ESP_I2S.h"
#include "es8311.h"
#include "waveshare-s3-touch-lcd-3.5b.hpp"
#include "beep.hpp"

// Pins and addresses from Waveshare's ESP32-S3-Touch-LCD-3.5B examples.

// LCD: AXS15231B, 320x480 portrait native, QSPI
#define LCD_QSPI_CS   12
#define LCD_QSPI_CLK  5
#define LCD_QSPI_D0   1
#define LCD_QSPI_D1   2
#define LCD_QSPI_D2   3
#define LCD_QSPI_D3   4
#define LCD_BL        6
#define LCD_WIDTH     320
#define LCD_HEIGHT    480

// Shared I2C bus: touch, TCA9554 IO expander, AXP2101, RTC, IMU, codec
#define I2C_SDA       8
#define I2C_SCL       7

// TCA9554 IO expander; EXIO1 drives the reset of the LCD and touch controller
#define TCA9554_ADDR        0x20
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_CONFIG  0x03
#define EXIO_LCD_RST        1

// Touch controller integrated in the AXS15231B
#define TOUCH_ADDR    0x3B

// Audio: ES8311 codec (configured over I2C) fed through I2S, drives the speaker
#define I2S_MCK_PIN   44
#define I2S_BCK_PIN   13
#define I2S_LRCK_PIN  15
#define I2S_DOUT_PIN  16
#define I2S_DIN_PIN   14

// Landscape to match the 480x320 UI: 1 or 3 (use the other if upside down).
#define DISPLAY_ROTATION 3

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_CLK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
static Arduino_GFX *panel = new Arduino_AXS15231B(bus, -1 /* RST via expander */, 0, false, LCD_WIDTH, LCD_HEIGHT);
// The AXS15231B does not handle partial window writes reliably over QSPI, so
// LVGL draws into this full-frame canvas, which is pushed to the panel in one
// go once LVGL has finished a refresh. The canvas also applies the rotation.
static Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel, 0, 0, DISPLAY_ROTATION);

// lv debugging can be set in lv_conf.h
#if LV_USE_LOG != 0
/* Serial debugging */
static void my_print(const char * buf)
{
    Serial.printf(buf);
    Serial.flush();
}
#endif

// create buffer for display; the canvas holds the full frame, so LVGL only
// needs a partial buffer
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 20];

/* Display flushing */
static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif
  if (lv_disp_flush_is_last(disp)) gfx->flush();
  lv_disp_flush_ready(disp);
}

static void tca9554Write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// Pulses EXIO1 of the TCA9554 to reset the LCD and touch controller.
// Only EXIO1 is made an output; the other expander pins stay inputs.
static void resetPanel() {
  const uint8_t rstMask = 1 << EXIO_LCD_RST;
  tca9554Write(TCA9554_REG_OUTPUT, 0xFF);
  tca9554Write(TCA9554_REG_CONFIG, (uint8_t)~rstMask);
  delay(10);
  tca9554Write(TCA9554_REG_OUTPUT, (uint8_t)~rstMask);
  delay(10);
  tca9554Write(TCA9554_REG_OUTPUT, 0xFF);
  delay(200);
}

// Last touch state reported by the controller, in native panel coordinates
// (320x480 portrait).
static bool touchPressed = false;
static uint16_t touchX = 0;
static uint16_t touchY = 0;

// Polls the touch controller and updates the state above. Like Waveshare's
// driver, frames without valid data (which the AXS15231B also returns while a
// finger is held) keep the previous state; only an explicit "no touch" frame
// releases. Releasing on every empty frame broke a long press into several
// short ones, so LV_EVENT_LONG_PRESSED never fired.
static void pollTouch() {
  static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};
  uint8_t data[14] = {0};

  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, sizeof(cmd));
  if (Wire.endTransmission() != 0) return;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)sizeof(data)) != sizeof(data)) return;
  Wire.readBytes(data, sizeof(data));

  // No new data in this frame: keep the previous state.
  if (data[1] == 0 || data[2] == 0 || data[3] < 2 || data[5] < 2) return;

  // Explicit release / invalid point count.
  if (data[0] == 0xff || data[1] > 2) {
    touchPressed = false;
    return;
  }

  uint16_t x = ((data[2] & 0x0F) << 8) | data[3];
  uint16_t y = ((data[4] & 0x0F) << 8) | data[5];
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
  touchX = x;
  touchY = y;
  touchPressed = true;
}

/*Read the touchpad*/
static void my_touch_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  pollTouch();
  if (!touchPressed) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  data->state = LV_INDEV_STATE_PR;
  uint16_t rawX = touchX;
  uint16_t rawY = touchY;
  // Map native portrait coordinates to the rotated (landscape) screen.
#if DISPLAY_ROTATION == 1
  data->point.x = rawY;
  data->point.y = (LCD_WIDTH - 1) - rawX;
#else
  data->point.x = (LCD_HEIGHT - 1) - rawY;
  data->point.y = rawX;
#endif

  #if DEBUG_TOUCH !=0
  Serial.print( "Data x " ); Serial.println( data->point.x );
  Serial.print( "Data y " ); Serial.println( data->point.y );
  #endif
}

void displayInit(){
  Wire.begin(I2C_SDA, I2C_SCL);
  resetPanel();

  if (!gfx->begin()) {
    Serial.println("Display init failed");
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->flush();

  pinMode(LCD_BL, OUTPUT);
  analogWrite(LCD_BL, 100);

  lv_init();
  #if LV_USE_LOG != 0
    lv_log_register_print_cb( my_print ); /* register print function for debugging */
  #endif

  // The draw buffer must be initialised before the driver that uses it.
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 20);
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
}

// ---- Key-press beep -------------------------------------------------------

#define BEEP_SAMPLE_RATE    16000
#define BEEP_MCLK_MULTIPLE  256
#define BEEP_FREQ_HZ        2000
#define BEEP_MS             40
#define BEEP_FADE_SAMPLES   80   // 5 ms fade in/out, avoids clicks
#define BEEP_AMPLITUDE      8000
#define BEEP_VOLUME         60   // codec volume, 0-100
#define BEEP_SAMPLES        (BEEP_SAMPLE_RATE * BEEP_MS / 1000)
#define BEEP_GAP_MS         120  // silence between the two tones of beepDouble()
#define BEEP_GAP_SAMPLES    (BEEP_SAMPLE_RATE * BEEP_GAP_MS / 1000)

// Notification bits sent to the beep task.
#define BEEP_SINGLE         (1 << 0)
#define BEEP_DOUBLE         (1 << 1)

static I2SClass i2s;
static int16_t beepBuffer[BEEP_SAMPLES * 2];     // interleaved stereo tone
static int16_t gapBuffer[BEEP_GAP_SAMPLES * 2];  // interleaved stereo silence
static TaskHandle_t beepTask = NULL;

static void playTone() {
  i2s.write((const uint8_t *)beepBuffer, sizeof(beepBuffer));
}

// Plays the requested beep whenever notified, so beep() never blocks the UI.
// If both were requested before it ran, the double beep wins.
static void beepTaskLoop(void *param) {
  uint32_t request;
  while (true) {
    xTaskNotifyWait(0, UINT32_MAX, &request, portMAX_DELAY);
    playTone();
    if (request & BEEP_DOUBLE) {
      i2s.write((const uint8_t *)gapBuffer, sizeof(gapBuffer));
      playTone();
    }
  }
}

void beepInit(void) {
  es8311_handle_t codec = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (!codec) {
    Serial.println("ES8311 not found, beep disabled");
    return;
  }
  const es8311_clock_config_t clk = {
    false,                                    // mclk_inverted
    false,                                    // sclk_inverted
    true,                                     // mclk_from_mclk_pin
    BEEP_SAMPLE_RATE * BEEP_MCLK_MULTIPLE,    // mclk_frequency
    BEEP_SAMPLE_RATE                          // sample_frequency
  };
  if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK ||
      es8311_voice_volume_set(codec, BEEP_VOLUME, NULL) != ESP_OK) {
    Serial.println("ES8311 init failed, beep disabled");
    return;
  }
  es8311_microphone_config(codec, false);

  i2s.setPins(I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_DIN_PIN, I2S_MCK_PIN);
  if (!i2s.begin(I2S_MODE_STD, BEEP_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S init failed, beep disabled");
    return;
  }

  // Precompute the tone with a short fade in/out.
  for (int n = 0; n < BEEP_SAMPLES; n++) {
    float envelope = 1.0f;
    if (n < BEEP_FADE_SAMPLES) envelope = (float)n / BEEP_FADE_SAMPLES;
    else if (n > BEEP_SAMPLES - BEEP_FADE_SAMPLES) envelope = (float)(BEEP_SAMPLES - n) / BEEP_FADE_SAMPLES;
    int16_t sample = (int16_t)(BEEP_AMPLITUDE * envelope * sinf(2.0f * PI * BEEP_FREQ_HZ * n / BEEP_SAMPLE_RATE));
    beepBuffer[2 * n] = sample;
    beepBuffer[2 * n + 1] = sample;
  }

  xTaskCreatePinnedToCore(beepTaskLoop, "beep", 2048, NULL, 2, &beepTask, 0);
}

void beep(void) {
  if (beepTask) xTaskNotify(beepTask, BEEP_SINGLE, eSetBits);
}

void beepDouble(void) {
  if (beepTask) xTaskNotify(beepTask, BEEP_DOUBLE, eSetBits);
}

#endif // BOARD_WAVESHARE_S3_TOUCH_LCD_35B
