#if defined(BOARD_JC3248W535)

#include "devices/i2cRecover.hpp"
#include "devices/boardPins.hpp"
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "ESP_I2S.h"
#include "beep.hpp"
#include "jc3248w535.hpp"
#include "taskCores.hpp"

// Display pins from Arduino_GFX's own definition of this board (JC3248W535
// in its Arduino_GFX_dev_device.h); touch bus from the board's published
// pinout.

// LCD: AXS15231B, 320x480 portrait native, QSPI. No reset line.
#define LCD_QSPI_CS   45
#define LCD_QSPI_CLK  47
#define LCD_QSPI_D0   21
#define LCD_QSPI_D1   48
#define LCD_QSPI_D2   40
#define LCD_QSPI_D3   39
#define LCD_BL        1    // HIGH = on
#define LCD_WIDTH     320
#define LCD_HEIGHT    480

// Touch controller integrated in the AXS15231B, on the board's internal I2C
// bus. That bus is not on any connector, so the moka's sensors use a second
// bus on the P3 / P4 connector (PIN_SENSOR_SDA / SCL in boardPins.hpp, Wire);
// the touch gets Wire1.
#define TOUCH_SDA     4
#define TOUCH_SCL     8
#define TOUCH_ADDR    0x3B
#define TouchWire     Wire1

// Audio: I2S amplifier driving the speaker connector. It takes I2S directly
// (no codec to set up, no MCLK), unlike the Waveshare's ES8311.
#define I2S_BCK_PIN   42
#define I2S_LRCK_PIN  2
#define I2S_DOUT_PIN  41

// Landscape to match the 480x320 UI: 1 or 3 (use the other if upside down).
#define DISPLAY_ROTATION 1

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_CLK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
// Init sequence "type1", the one Arduino_GFX uses for this board.
static Arduino_GFX *panel = new Arduino_AXS15231B(
    bus, GFX_NOT_DEFINED /* RST */, 0, false, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0,
    axs15231b_320480_type1_init_operations, sizeof(axs15231b_320480_type1_init_operations));
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

// Last touch state reported by the controller, in native panel coordinates
// (320x480 portrait).
// Written by the touch task, read by LVGL's read callback (other core).
static volatile bool touchPressed = false;
static volatile uint16_t touchX = 0;
static volatile uint16_t touchY = 0;

// Polls the touch controller and updates the state above (same controller
// and protocol as the Waveshare ESP32-S3-Touch-LCD-3.5B). Like Waveshare's
// driver, frames without valid data (which the AXS15231B also returns while a
// finger is held) keep the previous state; only an explicit "no touch" frame
// releases. Releasing on every empty frame broke a long press into several
// short ones, so LV_EVENT_LONG_PRESSED never fired.
// Not verified on this board yet: if a press never ends here (this panel
// reporting a lifted finger as a frame with no points), the "keep the
// previous state" test below is the place to change.
static void pollTouch() {
  static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};
  uint8_t data[14] = {0};

  TouchWire.beginTransmission(TOUCH_ADDR);
  TouchWire.write(cmd, sizeof(cmd));
  if (TouchWire.endTransmission() != 0) return;
  if (TouchWire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)sizeof(data)) != sizeof(data)) return;
  TouchWire.readBytes(data, sizeof(data));

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

// Touch is polled by its own task (touchTaskLoop, pinned to CORE_I2C with the
// other I2C users) instead of LVGL's read callback, which runs in the UI task
// on the other core. The task records every press/release transition in
// touchQueue and LVGL drains it, so a tap that starts and ends while LVGL is
// busy drawing is still delivered.
#define TOUCH_POLL_MS        75
#define TOUCH_QUEUE_LENGTH   16
#define TOUCH_TASK_PRIORITY  8   // above the sensor tasks: short and periodic

typedef struct {
  bool pressed;
  uint16_t x;  // native panel coordinates (320x480 portrait)
  uint16_t y;
} TouchEvent;

static QueueHandle_t touchQueue = NULL;
static TaskHandle_t touchTask = NULL;

// Polls every TOUCH_POLL_MS and queues each press/release transition.
static void touchTaskLoop(void *param) {
  bool lastQueued = false;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  while (true) {
    pollTouch();
    if (touchPressed != lastQueued) {
      TouchEvent event = { touchPressed, touchX, touchY };
      // Never block the poller; if LVGL falls 16 transitions behind, drop.
      if (xQueueSend(touchQueue, &event, 0) == pdTRUE) lastQueued = touchPressed;
    }
    xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(TOUCH_POLL_MS));
  }
}

/*Read the touchpad*/
static void my_touch_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  // State last handed to LVGL; persists between calls.
  static TouchEvent current = { false, 0, 0 };

  TouchEvent event;
  if (xQueueReceive(touchQueue, &event, 0) == pdTRUE) {
    current = event;
  }
  else if (current.pressed && touchPressed) {
    // No transition pending: follow the finger while it is held.
    current.x = touchX;
    current.y = touchY;
  }
  // More transitions queued (e.g. a whole tap since the last read): have LVGL
  // call again right away so the press and the release are both processed.
  data->continue_reading = uxQueueMessagesWaiting(touchQueue) > 0;

  data->state = current.pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  // Map native portrait coordinates to the rotated (landscape) screen. The
  // release keeps the last pressed position, as LVGL expects.
#if DISPLAY_ROTATION == 1
  data->point.x = current.y;
  data->point.y = (LCD_WIDTH - 1) - current.x;
#else
  data->point.x = (LCD_HEIGHT - 1) - current.y;
  data->point.y = current.x;
#endif

  #if DEBUG_TOUCH !=0
  Serial.print( "Data x " ); Serial.println( data->point.x );
  Serial.print( "Data y " ); Serial.println( data->point.y );
  #endif
}

void displayInit(){
  // Sensors (ADS1115, BMP280) on the P3 / P4 connector.
  i2cBusRecover(PIN_SENSOR_SDA, PIN_SENSOR_SCL);
  Wire.begin(PIN_SENSOR_SDA, PIN_SENSOR_SCL);
  // Touch on the internal bus.
  i2cBusRecover(TOUCH_SDA, TOUCH_SCL);
  TouchWire.begin(TOUCH_SDA, TOUCH_SCL);
  TouchWire.setTimeOut(I2C_TIMEOUT_MS);

  if (!gfx->begin()) {
    Serial.println("Display init failed");
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->flush();

  pinMode(LCD_BL, OUTPUT);
  analogWrite(LCD_BL, 200);

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

  touchQueue = xQueueCreate(TOUCH_QUEUE_LENGTH, sizeof(TouchEvent));
  xTaskCreatePinnedToCore(touchTaskLoop, "touch", 3072, NULL, TOUCH_TASK_PRIORITY, &touchTask, CORE_I2C);
}


// ---- Direct access for the Game Boy easter egg (src/games/gameboy.cpp) ---

Arduino_GFX *displayGameBegin() {
  // The game reads the touch controller itself, every frame and with both
  // points: the LVGL touch task stops.
  if (touchTask) vTaskSuspend(touchTask);
  TouchWire.setClock(400000);
  return gfx;
}

void displayGameBlit(const uint8_t *shades, int16_t srcW, int16_t srcH, int16_t gameX, int16_t gy,
                     const uint8_t *overlay, const uint8_t *columnHasOverlay,
                     const uint16_t (*colors)[5], bool redrawAll) {
  // Straight into the canvas's frame buffer, which is stored in the panel's
  // native portrait order: a screen column (landscape) is a row in memory.
  // Going column by column keeps the writes to PSRAM sequential; drawing line
  // by line through the canvas scattered every pixel to another cache line
  // (that took longer than the emulation itself).
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  const int16_t height = srcH * 2;
  for (int16_t X = 0; X < screenWidth; X++) {
    int16_t gameCol = X - gameX;
    bool inGame = gameCol >= 0 && gameCol < srcW * 2;
    bool masked = columnHasOverlay[X];
    // Outside the game only the controls change, and only with the colours.
    if (!inGame && !(masked && redrawAll)) continue;
    const uint8_t *column = shades + (inGame ? (gameCol >> 1) : 0);
    const uint8_t *mask = overlay + (int32_t)X * height;
#if DISPLAY_ROTATION == 1
    uint16_t *out = fb + (int32_t)X * LCD_WIDTH + (LCD_WIDTH - 1 - gy);
    const int step = -1;
#else
    uint16_t *out = fb + (int32_t)(LCD_HEIGHT - 1 - X) * LCD_WIDTH + gy;
    const int step = 1;
#endif
    if (!masked) {
      // Game only: no mask to read.
      const uint16_t *plain = colors[0];
      for (int16_t row = 0; row < height; row += 2, column += srcW) {
        uint16_t colour = plain[*column & 0x03];
        out[0] = colour;
        out[step] = colour;
        out += 2 * step;
      }
    }
    else {
      for (int16_t row = 0; row < height; row++, out += step) {
        uint8_t base = inGame ? (column[(row >> 1) * srcW] & 0x03) : 4;
        *out = colors[mask[row]][base];
      }
    }
  }
}

int displayReadTouchPoints(int16_t *xs, int16_t *ys, int max) {
  static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};
  uint8_t data[14] = {0};
  TouchWire.beginTransmission(TOUCH_ADDR);
  TouchWire.write(cmd, sizeof(cmd));
  if (TouchWire.endTransmission() != 0) return -1;
  if (TouchWire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)sizeof(data)) != sizeof(data)) return -1;
  TouchWire.readBytes(data, sizeof(data));

  // Same frame checks as pollTouch().
  if (data[1] == 0 || data[2] == 0 || data[3] < 2 || data[5] < 2) return -1;
  if (data[0] == 0xff || data[1] > 2) return 0;

  int count = 0;
  for (int k = 0; k < data[1] && count < max; k++) {
    const uint8_t *point = data + 2 + 6 * k;
    uint16_t x = ((point[0] & 0x0F) << 8) | point[1];
    uint16_t y = ((point[2] & 0x0F) << 8) | point[3];
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) continue;
    // Native portrait -> rotated landscape, as in my_touch_read().
#if DISPLAY_ROTATION == 1
    xs[count] = y;
    ys[count] = (LCD_WIDTH - 1) - x;
#else
    xs[count] = (LCD_HEIGHT - 1) - y;
    ys[count] = x;
#endif
    count++;
  }
  return count;
}

// ---- Key-press beep -------------------------------------------------------
// Same tones as the Waveshare board: a 2 kHz, 40 ms beep with 5 ms fades,
// played by a task so beep() never blocks the UI.

#define BEEP_SAMPLE_RATE    16000
#define BEEP_FREQ_HZ        2000
#define BEEP_MS             40
#define BEEP_FADE_SAMPLES   80   // 5 ms fade in/out, avoids clicks
// No codec volume here: the amplifier plays the samples as they are, so the
// level is set by the amplitude (out of 32767).
#define BEEP_AMPLITUDE      6000
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

// Plays the requested beep whenever notified. If both were requested before
// it ran, the double beep wins.
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
  i2s.setPins(I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
  if (!i2s.begin(I2S_MODE_STD, BEEP_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S init failed, beep disabled");
    return;
  }

  // Precompute the tone with a short fade in/out, on both channels: a mono
  // amplifier plays one of them, depending on how it is wired.
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

#endif // BOARD_JC3248W535
