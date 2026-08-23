#include "font5x7.h"

namespace {

struct Glyph {
  char c;
  uint8_t rows[7];  // bits 4..0 = columns left..right, 1 = lit
};

// clang-format off
constexpr Glyph kFont5x7[] = {
    // ' '
    {' ', {0b00000,
           0b00000,
           0b00000,
           0b00000,
           0b00000,
           0b00000,
           0b00000}},
    // '!'
    {'!', {0b00100,
           0b00100,
           0b00100,
           0b00100,
           0b00100,
           0b00000,
           0b00100}},
    // ','
    {',', {0b00000,
           0b00000,
           0b00000,
           0b00000,
           0b00110,
           0b00100,
           0b01000}},
    // 'H'
    {'H', {0b10001,
           0b10001,
           0b10001,
           0b11111,
           0b10001,
           0b10001,
           0b10001}},
    // 'd'
    {'d', {0b00001,
           0b00001,
           0b01111,
           0b10001,
           0b10001,
           0b10001,
           0b01111}},
    // 'e'
    {'e', {0b00000,
           0b01110,
           0b10001,
           0b11111,
           0b10000,
           0b10001,
           0b01110}},
    // 'l'
    {'l', {0b00100,
           0b00100,
           0b00100,
           0b00100,
           0b00100,
           0b00100,
           0b00100}},
    // 'o'
    {'o', {0b00000,
           0b01110,
           0b10001,
           0b10001,
           0b10001,
           0b10001,
           0b01110}},
    // 'r'
    {'r', {0b00000,
           0b00000,
           0b01110,
           0b10000,
           0b10000,
           0b10000,
           0b10000}},
    // 'w'
    {'w', {0b00000,
           0b00000,
           0b10001,
           0b10001,
           0b10101,
           0b11011,
           0b10001}},
};
// clang-format on

const Glyph *find_glyph(char c) {
  for (const auto &g : kFont5x7) {
    if (g.c == c) {
      return &g;
    }
  }
  return nullptr;
}

}  // namespace

void font5x7_draw_char(Hub75Driver &driver, int16_t x, int16_t y, char c, uint8_t r, uint8_t g, uint8_t b) {
  const Glyph *glyph = find_glyph(c);
  if (glyph == nullptr) {
    return;  // unmapped character -> blank cell
  }

  const int16_t width = driver.get_width();
  const int16_t height = driver.get_height();

  for (int row = 0; row < 7; row++) {
    const uint8_t bits = glyph->rows[row];
    const int16_t py = y + row;
    if (py < 0 || py >= height) {
      continue;
    }
    for (int col = 0; col < 5; col++) {
      if ((bits & (1 << (4 - col))) == 0) {
        continue;
      }
      const int16_t px = x + col;
      if (px < 0 || px >= width) {
        continue;
      }
      driver.set_pixel(px, py, r, g, b);
    }
  }
}

void font5x7_draw_text(Hub75Driver &driver, int16_t x, int16_t y, const char *text, uint8_t r, uint8_t g,
                        uint8_t b) {
  int16_t cursor = x;
  for (const char *p = text; *p != '\0'; p++) {
    font5x7_draw_char(driver, cursor, y, *p, r, g, b);
    cursor += kFont5x7Advance;
  }
}
