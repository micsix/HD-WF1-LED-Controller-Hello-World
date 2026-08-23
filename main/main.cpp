#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

static const char *TAG = "hub75_hello";

// Huidu HD-WF1 (ESP32-S2) onboard HUB75E pinout.
// Source: mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA, src/hd-wf1-esp32s2-config.h
//
// G1/B1 and G2/B2 swapped vs. that source: empirically confirmed on this
// physical panel via solid-color tests - pure green data lit up blue LEDs
// and pure blue data lit up green LEDs (red was correct). Swapping the GPIO
// numbers here (not the code) compensates so software color now matches
// physical color.
#define WF1_R1_PIN 2
#define WF1_R2_PIN 4
#define WF1_G1_PIN 3
#define WF1_G2_PIN 5
#define WF1_B1_PIN 6
#define WF1_B2_PIN 8
#define WF1_A_PIN  39
#define WF1_B_PIN  38
#define WF1_C_PIN  37
#define WF1_D_PIN  36
#define WF1_E_PIN  12
#define WF1_OE_PIN  35
#define WF1_CLK_PIN 34
#define WF1_LAT_PIN 33

// Confirmed: 64x32 HUB75E panel, single chain.
#define PANEL_WIDTH  64
#define PANEL_HEIGHT 32
#define PANEL_CHAIN  1

static MatrixPanel_I2S_DMA *dma_display = nullptr;

// Minimal built-in 5x7 font (NO_GFX build has no Adafruit-GFX print()/text
// API - drawPixelRGB888 is used directly). Only the glyphs needed for the
// demo string are populated; extend kFont5x7 for more coverage.
namespace {

struct Glyph {
  char c;
  uint8_t rows[7];  // bits 4..0 = columns left..right, 1 = lit
};

// clang-format off
constexpr Glyph kFont5x7[] = {
    {' ', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
    {',', {0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00100, 0b01000}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'d', {0b00001, 0b00001, 0b01111, 0b10001, 0b10001, 0b10001, 0b01111}},
    {'e', {0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b10001, 0b01110}},
    {'l', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'o', {0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'r', {0b00000, 0b00000, 0b01110, 0b10000, 0b10000, 0b10000, 0b10000}},
    {'w', {0b00000, 0b00000, 0b10001, 0b10001, 0b10101, 0b11011, 0b10001}},
};
// clang-format on

constexpr int16_t kFontAdvance = 6;  // 5px glyph + 1px spacing

const Glyph *find_glyph(char c) {
  for (const auto &g : kFont5x7) {
    if (g.c == c) return &g;
  }
  return nullptr;
}

void draw_char(int16_t x, int16_t y, char c, uint8_t r, uint8_t g, uint8_t b) {
  const Glyph *glyph = find_glyph(c);
  if (glyph == nullptr) return;
  for (int row = 0; row < 7; row++) {
    const int16_t py = y + row;
    if (py < 0 || py >= PANEL_HEIGHT) continue;
    const uint8_t bits = glyph->rows[row];
    for (int col = 0; col < 5; col++) {
      if ((bits & (1 << (4 - col))) == 0) continue;
      const int16_t px = x + col;
      if (px < 0 || px >= PANEL_WIDTH) continue;
      dma_display->drawPixelRGB888(px, py, r, g, b);
    }
  }
}

void draw_text(int16_t x, int16_t y, const char *text, uint8_t r, uint8_t g, uint8_t b) {
  int16_t cursor = x;
  for (const char *p = text; *p != '\0'; p++) {
    draw_char(cursor, y, *p, r, g, b);
    cursor += kFontAdvance;
  }
}

}  // namespace

extern "C" void app_main(void)
{
    HUB75_I2S_CFG::i2s_pins pins = {
        WF1_R1_PIN, WF1_G1_PIN, WF1_B1_PIN, WF1_R2_PIN, WF1_G2_PIN, WF1_B2_PIN,
        WF1_A_PIN, WF1_B_PIN, WF1_C_PIN, WF1_D_PIN, WF1_E_PIN,
        WF1_LAT_PIN, WF1_OE_PIN, WF1_CLK_PIN
    };

    HUB75_I2S_CFG mxconfig(PANEL_WIDTH, PANEL_HEIGHT, PANEL_CHAIN, pins);
    mxconfig.driver = HUB75_I2S_CFG::FM6126A;
    mxconfig.latch_blanking = 4;

    ESP_LOGI(TAG, "constructing MatrixPanel_I2S_DMA: %dx%d chain=%d driver=FM6126A latch_blanking=4",
             PANEL_WIDTH, PANEL_HEIGHT, PANEL_CHAIN);

    dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    bool ok = dma_display->begin();
    ESP_LOGI(TAG, "dma_display->begin() returned %d", ok);
    if (!ok) {
        ESP_LOGE(TAG, "begin() FAILED");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    dma_display->setBrightness8(180);

    const char *msg = "Hello, world!";
    const auto text_px = static_cast<int16_t>(strlen(msg) * kFontAdvance);
    const int16_t y = (PANEL_HEIGHT - 7) / 2;
    int16_t x = PANEL_WIDTH;

    while (true) {
        dma_display->clearScreen();
        draw_text(x, y, msg, 0, 255, 0);

        if (--x < -text_px) {
            x = PANEL_WIDTH;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
