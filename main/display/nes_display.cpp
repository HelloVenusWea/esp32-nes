#include "nes_display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "board_pins.h"

static const char* TAG = "NesDisplay";

namespace nesdisplay {
namespace {

constexpr int kNesW = board::kNesWidth;
constexpr int kNesH = board::kNesHeight;

esp_lcd_panel_handle_t s_panel = nullptr;
int s_screen_w = 0;
int s_screen_h = 0;
int s_x_off = 0;
int s_y_off = 0;

SemaphoreHandle_t s_frame_sem = nullptr;  // new frame pending
SemaphoreHandle_t s_done_sem = nullptr;   // previous frame transferred
const uint16_t* s_pending = nullptr;

volatile bool s_running = false;
volatile uint32_t s_rendered = 0;
volatile uint32_t s_draw_total_us = 0;
TaskHandle_t s_task = nullptr;

uint16_t* s_bar = nullptr;

void DisplayTask(void* arg) {
    (void)arg;
    while (s_running) {
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        uint16_t* p = const_cast<uint16_t*>(s_pending);
        if (p == nullptr) {
            xSemaphoreGive(s_done_sem);
            continue;
        }

        /* InfoNES marks sprite pixels with bit 15; clear it, then swap bytes
         * because ST7789 over SPI expects big-endian RGB565. */
        for (int i = 0; i < kNesW * kNesH; i++) {
            uint16_t v = p[i] & 0x7FFF;
            p[i] = (uint16_t)((v >> 8) | (v << 8));
        }

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, s_x_off, s_y_off,
                                                  s_x_off + kNesW, s_y_off + kNesH, p);
        s_draw_total_us = s_draw_total_us + (uint32_t)(esp_timer_get_time() - t0);
        s_rendered = s_rendered + 1;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
        }
        xSemaphoreGive(s_done_sem);
    }
    s_task = nullptr;
    vTaskDelete(nullptr);
}

void DrawBars() {
    if (s_x_off <= 0) {
        return;
    }
    if (s_bar == nullptr) {
        s_bar = (uint16_t*)heap_caps_calloc(1, (size_t)s_x_off * kNesH * sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_bar == nullptr) {
        return;
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, s_y_off, s_x_off, s_y_off + kNesH, s_bar);
    esp_lcd_panel_draw_bitmap(s_panel, s_x_off + kNesW, s_y_off, s_screen_w,
                              s_y_off + kNesH, s_bar);
}

}  // namespace

bool Start(esp_lcd_panel_handle_t panel, int screen_width, int screen_height) {
    s_panel = panel;
    s_screen_w = screen_width;
    s_screen_h = screen_height;
    s_x_off = (screen_width - kNesW) / 2;
    s_y_off = (screen_height - kNesH) / 2;
    if (s_running) {
        return true;
    }
    if (s_panel == nullptr) {
        ESP_LOGE(TAG, "no panel");
        return false;
    }
    if (s_frame_sem == nullptr) {
        s_frame_sem = xSemaphoreCreateBinary();
    }
    if (s_done_sem == nullptr) {
        s_done_sem = xSemaphoreCreateBinary();
        xSemaphoreGive(s_done_sem);
    }
    s_rendered = 0;
    s_draw_total_us = 0;
    s_pending = nullptr;
    s_running = true;
    DrawBars();
    if (xTaskCreatePinnedToCore(DisplayTask, "nes_display", 4096, nullptr, 4, &s_task, 0) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "display task creation failed");
        return false;
    }
    ESP_LOGI(TAG, "game area %dx%d at (%d,%d) on %dx%d", kNesW, kNesH, s_x_off, s_y_off,
             screen_width, screen_height);
    return true;
}

void Stop() {
    if (!s_running) {
        return;
    }
    s_running = false;
    if (s_frame_sem != nullptr) {
        xSemaphoreGive(s_frame_sem);
    }
    for (int i = 0; i < 100 && s_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "stopped: %u frames, avg flush %.1f ms", (unsigned)s_rendered,
             s_rendered ? s_draw_total_us / 1000.0 / s_rendered : 0.0);
}

void SubmitFrame(const uint16_t* frame) {
    if (!s_running) {
        return;
    }
    /* Wait for the previous transfer so the emulator can reuse that buffer. */
    xSemaphoreTake(s_done_sem, portMAX_DELAY);
    s_pending = frame;
    xSemaphoreGive(s_frame_sem);
}

uint32_t RenderedFrames() {
    return s_rendered;
}

uint32_t AverageDrawMs() {
    uint32_t n = s_rendered;
    return n == 0 ? 0 : (uint32_t)(s_draw_total_us / n / 1000);
}

}  // namespace nesdisplay
