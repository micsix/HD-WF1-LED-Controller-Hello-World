#pragma once
#include <cstdint>
#include "hub75.h"

// Minimal, dependency-free 5x7 bitmap font renderer for Hub75Driver.
// Only the glyphs needed for the demo string are populated below;
// unmapped characters render as a blank cell. Add rows to kFont5x7 in
// font5x7.cpp to extend coverage.

constexpr int16_t kFont5x7Advance = 6;  // 5px glyph + 1px spacing

void font5x7_draw_char(Hub75Driver &driver, int16_t x, int16_t y, char c, uint8_t r, uint8_t g, uint8_t b);

void font5x7_draw_text(Hub75Driver &driver, int16_t x, int16_t y, const char *text, uint8_t r, uint8_t g,
                        uint8_t b);
