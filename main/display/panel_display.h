#pragma once

#include <cstdint>

#include <esp_err.h>
#include <esp_lcd_panel_ops.h>

namespace display {

/* RGB565 with bytes swapped: ST7789 over SPI expects big-endian pixel data.
 * The menu canvas is kept in this on-wire format so it can be flushed directly. */
uint16_t Rgb565(uint8_t r, uint8_t g, uint8_t b);

bool InitCanvas(esp_lcd_panel_handle_t panel, int width, int height);
void Clear(uint16_t color);
void FillRect(int x, int y, int w, int h, uint16_t color);

/* 8x8 ASCII font, scaled by `scale` (1 or 2). Non-ASCII UTF-8 renders as '?'. */
void DrawText(int x, int y, const char* text, uint16_t fg, uint16_t bg, int scale = 2,
              int max_chars = -1);

/* Mixed ASCII + CJK (GB2312 subset) 16px-high bitmap font. ASCII is 8x16,
 * Chinese is 16x16. Returns the pixel width drawn; truncates at max_px. */
int DrawTextCjk(int x, int y, const char* text, uint16_t fg, uint16_t bg, int scale = 1,
                int max_px = 0);

void FlushAll();
void FlushRect(int x, int y, int w, int h);

/* Current on-wire RGB565 canvas (big-endian pixels), for a debug screenshot. */
const uint16_t* CanvasData(int* width, int* height);

}  // namespace display
