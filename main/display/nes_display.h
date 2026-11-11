#pragma once

#include <cstdint>

#include <esp_lcd_panel_ops.h>

namespace nesdisplay {

/* 256x240 NES frames drawn directly to the ST7789, centered horizontally.
 * Double buffering is provided by InfoNES (DoubleFrame[2] in PSRAM). */
bool Start(esp_lcd_panel_handle_t panel, int screen_width, int screen_height);
void Stop();

void SubmitFrame(const uint16_t* frame);

uint32_t RenderedFrames();
uint32_t AverageDrawMs();

}  // namespace nesdisplay
