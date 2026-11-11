#include "panel_display.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "font8x8_basic.h"
#include "font_cjk.h"

static const char* TAG = "Display";

namespace display {
namespace {

esp_lcd_panel_handle_t s_panel = nullptr;
int s_width = 0;
int s_height = 0;
uint16_t* s_canvas = nullptr;

inline void PutPixel(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= s_width || y >= s_height) {
        return;
    }
    s_canvas[y * s_width + x] = color;
}

const CjkGlyph* FindGlyph(uint32_t cp) {
    int lo = 0;
    int hi = kCjkGlyphCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kCjkGlyphs[mid].cp == cp) {
            return &kCjkGlyphs[mid];
        }
        if (kCjkGlyphs[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return nullptr;
}

uint32_t NextUtf8(const unsigned char*& p) {
    unsigned c = *p++;
    if (c < 0x80) {
        return c;
    }
    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07;
    } else {
        return '?';
    }
    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) {
            return '?';
        }
        cp = (cp << 6) | (*p & 0x3F);
        p++;
    }
    return cp;
}

}  // namespace

uint16_t Rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

bool InitCanvas(esp_lcd_panel_handle_t panel, int width, int height) {
    s_panel = panel;
    s_width = width;
    s_height = height;
    if (s_canvas == nullptr) {
        s_canvas = (uint16_t*)heap_caps_aligned_alloc(
            32, (size_t)width * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_canvas == nullptr) {
        ESP_LOGE(TAG, "canvas allocation failed (%dx%d)", width, height);
        return false;
    }
    ESP_LOGI(TAG, "menu canvas %dx%d in PSRAM", width, height);
    return true;
}

void Clear(uint16_t color) {
    if (s_canvas == nullptr) {
        return;
    }
    for (int i = 0; i < s_width * s_height; i++) {
        s_canvas[i] = color;
    }
}

void FillRect(int x, int y, int w, int h, uint16_t color) {
    if (s_canvas == nullptr) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > s_width ? s_width : x + w;
    int y1 = y + h > s_height ? s_height : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t* row = s_canvas + (size_t)yy * s_width;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

void DrawText(int x, int y, const char* text, uint16_t fg, uint16_t bg, int scale,
              int max_chars) {
    if (s_canvas == nullptr || text == nullptr) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    int cursor = x;
    int cell = 8 * scale;
    int count = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p != 0 && (max_chars < 0 || count < max_chars);) {
        unsigned char c = *p;
        if (c < 0x80) {
            p++;
        } else {
            /* Skip one UTF-8 sequence, render a placeholder. */
            p++;
            while ((*p & 0xC0) == 0x80) {
                p++;
            }
            c = '?';
        }
        const char* glyph = font8x8_basic[c & 0x7F];
        for (int gy = 0; gy < 8; gy++) {
            unsigned char bits = (unsigned char)glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                uint16_t color = (bits & (1u << gx)) ? fg : bg;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        PutPixel(cursor + gx * scale + sx, y + gy * scale + sy, color);
                    }
                }
            }
        }
        cursor += cell;
        count++;
    }
}

int DrawTextCjk(int x, int y, const char* text, uint16_t fg, uint16_t bg, int scale,
                int max_px) {
    if (s_canvas == nullptr || text == nullptr) {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    const int start = x;
    const int limit = max_px > 0 ? x + max_px : s_width;
    const unsigned char* p = (const unsigned char*)text;
    while (*p != 0) {
        uint32_t cp = NextUtf8(p);
        const CjkGlyph* g = FindGlyph(cp);
        if (g == nullptr) {
            g = FindGlyph('?');
        }
        if (g == nullptr) {
            break;
        }
        const int w = g->width * scale;
        if (x + w > limit) {
            break;
        }
        for (int row = 0; row < 16; row++) {
            const uint8_t left = g->bitmap[row * 2];
            const uint8_t right = g->bitmap[row * 2 + 1];
            for (int col = 0; col < g->width; col++) {
                const bool on = col < 8 ? ((left >> (7 - col)) & 1) != 0
                                        : ((right >> (15 - col)) & 1) != 0;
                const uint16_t color = on ? fg : bg;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        PutPixel(x + col * scale + sx, y + row * scale + sy, color);
                    }
                }
            }
        }
        x += w;
    }
    return x - start;
}

void FlushAll() {
    FlushRect(0, 0, s_width, s_height);
}void FlushRect(int x, int y, int w, int h) {
    if (s_panel == nullptr || s_canvas == nullptr) {
        return;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h,
                                              s_canvas + (size_t)y * s_width + x);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
    }
}

const uint16_t* CanvasData(int* width, int* height) {
    if (width != nullptr) {
        *width = s_width;
    }
    if (height != nullptr) {
        *height = s_height;
    }
    return s_canvas;
}

}  // namespace display
