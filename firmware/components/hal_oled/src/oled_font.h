#pragma once

// Tiny 5x7 ASCII font (printable range 0x20-0x7E). Each glyph is 5
// vertical-byte columns; bit 0 of each column is the top pixel. Rendered
// 1 column-gap to the right → effective 6x8 cell.

#include <stdint.h>

extern const uint8_t oled_font_5x7[95][5];
