#include "nes_audio.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio_out.h"
#include "nes_system.h"

static const char* TAG = "NesAudio";

namespace nesaudio {
namespace {

constexpr int kSampleRate = 24000;
constexpr int kChunkMs = 20;
constexpr int kChunkSamples = kSampleRate * kChunkMs / 1000;  // 480

volatile bool s_running = false;
volatile uint32_t s_pushed = 0;
volatile uint32_t s_underruns = 0;
TaskHandle_t s_task = nullptr;

void AudioTask(void* arg) {
    (void)arg;
    int16_t* buf = (int16_t*)heap_caps_malloc(kChunkSamples * sizeof(int16_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        buf = (int16_t*)heap_caps_malloc(kChunkSamples * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == nullptr) {
        ESP_LOGE(TAG, "audio buffer allocation failed");
        s_running = false;
        s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    TickType_t next_wake = xTaskGetTickCount();
    int64_t last_stats = esp_timer_get_time();
    uint32_t last_underruns = 0;
    while (s_running) {
        uint32_t n = NesSys_AudioRead(buf, kChunkSamples);
        if (n < (uint32_t)kChunkSamples) {
            s_underruns = s_underruns + 1;
            /* Conceal with the last valid sample instead of a hard zero to
             * avoid clicks when the emulator briefly falls behind. */
            const int16_t last = n > 0 ? buf[n - 1] : 0;
            for (uint32_t i = n; i < (uint32_t)kChunkSamples; i++) {
                buf[i] = last;
            }
        }
        size_t written = audio::Write(buf, kChunkSamples);
        if (written == 0 && s_running) {
            ESP_LOGW(TAG, "audio write failed");
            vTaskDelay(pdMS_TO_TICKS(kChunkMs));
            continue;
        }
        s_pushed = s_pushed + n;

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_stats >= 5 * 1000 * 1000) {
            const uint32_t delta = s_underruns - last_underruns;
            if (delta > 0) {
                ESP_LOGW(TAG, "5 s: %u underruns, ring %u samples", (unsigned)delta,
                         (unsigned)NesSys_AudioAvailable());
            }
            last_underruns = s_underruns;
            last_stats = now_us;
        }

        next_wake += pdMS_TO_TICKS(kChunkMs);
        TickType_t now = xTaskGetTickCount();
        if (next_wake <= now) {
            next_wake = now + pdMS_TO_TICKS(kChunkMs);
        }
        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(kChunkMs));
    }

    free(buf);
    s_task = nullptr;
    ESP_LOGI(TAG, "audio pump stopped, %u samples pushed", (unsigned)s_pushed);
    vTaskDelete(nullptr);
}

}  // namespace

bool Start() {
    if (s_running) {
        return true;
    }
    s_pushed = 0;
    s_running = true;
    if (xTaskCreatePinnedToCore(AudioTask, "nes_audio", 4096, nullptr, 3, &s_task, 0) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "audio task creation failed");
        return false;
    }
    ESP_LOGI(TAG, "audio pump started: %d Hz, %d ms chunks", kSampleRate, kChunkMs);
    return true;
}

void Stop() {
    if (!s_running) {
        return;
    }
    s_running = false;
    for (int i = 0; i < 50 && s_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "%u samples pushed", (unsigned)s_pushed);
}

uint32_t PushedSamples() {
    return s_pushed;
}

}  // namespace nesaudio
