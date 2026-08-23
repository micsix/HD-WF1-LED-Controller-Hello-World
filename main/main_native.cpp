#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub75.h"

#include "font5x7.h"

static const char *TAG = "hub75_hello";

// Huidu HD-WF1 (ESP32-S2) onboard HUB75E pinout.
// Source: mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA, src/hd-wf1-esp32s2-config.h
#define WF1_R1_PIN 2
#define WF1_R2_PIN 4
#define WF1_G1_PIN 6
#define WF1_G2_PIN 8
#define WF1_B1_PIN 3
#define WF1_B2_PIN 5
#define WF1_A_PIN 39
#define WF1_B_PIN 38
#define WF1_C_PIN 37
#define WF1_D_PIN 36
#define WF1_E_PIN 12
#define WF1_OE_PIN 35
#define WF1_CLK_PIN 34
#define WF1_LAT_PIN 33

// Adjust these two to match the panel(s) actually chained onto the port.
#define PANEL_WIDTH 64
#define PANEL_HEIGHT 32

extern "C" void app_main(void) {
  Hub75Config config{};
  config.panel_width = PANEL_WIDTH;
  config.panel_height = PANEL_HEIGHT;
  config.brightness = 255;
  // Panel is a 64x32 HUB75E module - that form factor is very commonly built
  // on FM6126A shift registers (they stay fully blanked without their
  // driver-IC init sequence, which GENERIC never sends).
  config.shift_driver = Hub75ShiftDriver::FM6126A;
  // WF1's own known-working reference firmware also sets latch_blanking=4
  // (library default is 1) - untested until now in combination with FM6126A.
  config.latch_blanking = 4;

  config.pins.r1 = WF1_R1_PIN;
  config.pins.g1 = WF1_G1_PIN;
  config.pins.b1 = WF1_B1_PIN;
  config.pins.r2 = WF1_R2_PIN;
  config.pins.g2 = WF1_G2_PIN;
  config.pins.b2 = WF1_B2_PIN;
  config.pins.a = WF1_A_PIN;
  config.pins.b = WF1_B_PIN;
  config.pins.c = WF1_C_PIN;
  config.pins.d = WF1_D_PIN;
  config.pins.e = WF1_E_PIN;
  config.pins.lat = WF1_LAT_PIN;
  config.pins.oe = WF1_OE_PIN;
  config.pins.clk = WF1_CLK_PIN;

  ESP_LOGI(TAG, "starting Hub75Driver: %ux%u, pins r1=%d g1=%d b1=%d r2=%d g2=%d b2=%d "
                "a=%d b=%d c=%d d=%d e=%d lat=%d oe=%d clk=%d",
           config.panel_width, config.panel_height, config.pins.r1, config.pins.g1, config.pins.b1,
           config.pins.r2, config.pins.g2, config.pins.b2, config.pins.a, config.pins.b, config.pins.c,
           config.pins.d, config.pins.e, config.pins.lat, config.pins.oe, config.pins.clk);

  Hub75Driver driver(config);
  if (!driver.begin()) {
    ESP_LOGE(TAG, "Hub75Driver::begin() FAILED - check pin conflicts, DMA/IRAM availability, "
                   "and that scan_wiring/shift_driver match the attached panel");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
  ESP_LOGI(TAG, "Hub75Driver::begin() OK, running=%d, width=%u height=%u", driver.is_running(),
           driver.get_width(), driver.get_height());

#if 1
  // Minimal sanity test: bypass font/scroll logic entirely and just slam the
  // whole panel solid white at max brightness. If this doesn't light up
  // anything, the problem is upstream of drawing logic (wiring/signal/panel).
  ESP_LOGI(TAG, "SANITY MODE: solid full-panel white fill, no scrolling text");
  driver.fill(0, 0, driver.get_width(), driver.get_height(), 255, 255, 255);
  uint32_t frame = 0;
  while (true) {
    if (++frame % 100 == 0) {
      ESP_LOGI(TAG, "alive: frame=%u running=%d", frame, driver.is_running());
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
#else
  const char *msg = "Hello, world!";
  const auto text_px = static_cast<int16_t>(strlen(msg) * kFont5x7Advance);
  const int16_t y = (driver.get_height() - 7) / 2;
  int16_t x = driver.get_width();

  uint32_t frame = 0;
  while (true) {
    driver.clear();
    font5x7_draw_text(driver, x, y, msg, 0, 255, 0);

    if (--x < -text_px) {
      x = driver.get_width();
    }

    if (++frame % 100 == 0) {
      ESP_LOGI(TAG, "alive: frame=%u x=%d running=%d", frame, x, driver.is_running());
    }

    vTaskDelay(pdMS_TO_TICKS(30));
  }
#endif
}
