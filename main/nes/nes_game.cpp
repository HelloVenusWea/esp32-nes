#include "nes_game.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "InfoNES.h"
#include "audio_out.h"
#include "board_init.h"
#include "board_pins.h"
#include "input.h"
#include "nes_audio.h"
#include "nes_display.h"
#include "nes_system.h"

static const char* TAG = "NesGame";

namespace nesgame {
namespace {

volatile bool s_monitor_running = false;
TaskHandle_t s_monitor_task = nullptr;
volatile bool s_core_done = false;

/* InfoNES_Cycle() is the hot loop. Run it on core 1 so Wi-Fi/BLE/audio on
 * core 0 cannot preempt it every few milliseconds. */
void CoreTask(void* arg) {
    (void)arg;
    InfoNES_Cycle();
    s_core_done = true;
    vTaskDelete(nullptr);
}

void MonitorTask(void* arg) {
    (void)arg;
    int64_t combo_start = 0;
    bool combo_active = false;
    int64_t start_hold = 0;
    bool start_active = false;
    bool start_armed = false;
    bool combo_armed = false;
    int64_t last_report = esp_timer_get_time();
    uint32_t last_frames = 0;
    while (s_monitor_running) {
        input::ScanResult scan = input::Scan();
        NesSys_SetPad(scan.pad);

        const int64_t now = esp_timer_get_time();
        if (now - last_report >= 10 * 1000 * 1000) {
            const uint32_t frames = NesSys_FrameCount();
            const double fps = (frames - last_frames) * 1000000.0 / (double)(now - last_report);
            ESP_LOGI(TAG, "%u frames, %.1f fps, audio %u samples buffered", (unsigned)frames, fps,
                     (unsigned)NesSys_AudioAvailable());
            last_report = now;
            last_frames = frames;
        }

        if (scan.boot_long) {
            ESP_LOGI(TAG, "BOOT long press, quitting");
            NesSys_RequestStop();
        }

        /* Fallback for pads without a working Select button: holding Start
         * for 1.5 s exits. A short Start press still reaches the game.
         * Require a release first, so starting a game by holding Start does
         * not immediately exit it. */
        if (!(scan.pad & input::kStart)) {
            start_armed = true;
            start_active = false;
        } else if (start_armed) {
            if (!start_active) {
                start_active = true;
                start_hold = now;
            } else if (now - start_hold > 1500 * 1000) {
                ESP_LOGI(TAG, "Start held 1.5 s, quitting");
                NesSys_RequestStop();
            }
        }

        const uint8_t combo = input::kSelect | input::kStart;
        if ((scan.pad & combo) != combo) {
            combo_armed = true;
        }
        if (combo_armed && (scan.pad & combo) == combo) {
            if (!combo_active) {
                combo_active = true;
                combo_start = now;
            } else if (now - combo_start > 700 * 1000) {
                ESP_LOGI(TAG, "Select+Start held, quitting");
                NesSys_RequestStop();
            }
        } else {
            combo_active = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_monitor_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool Run(const uint8_t* rom, size_t len, const char* display_name, uint32_t frame_limit,
         bool debug) {
    if (rom == nullptr || len < 16 || memcmp(rom, "NES\x1a", 4) != 0) {
        ESP_LOGE(TAG, "invalid ROM");
        return false;
    }
    ESP_LOGI(TAG, "loading %s (%u bytes)", display_name ? display_name : "?", (unsigned)len);
    ESP_LOGI(TAG, "heap before load: internal=%u largest=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    NesSys_ResetState();
    NesSys_SetRom(rom, len);
    NesSys_SetFrameLimit(frame_limit);
    NesSys_SetDebug(debug);
    InfoNES_Init();
    if (InfoNES_Load("") != 0) {
        ESP_LOGE(TAG, "InfoNES_Load failed");
        InfoNES_Fin();
        return false;
    }

    if (!audio::Start()) {
        ESP_LOGW(TAG, "audio output unavailable, continuing muted");
    }
    if (!nesdisplay::Start(board::Get().panel, board::kScreenWidth, board::kScreenHeight)) {
        ESP_LOGE(TAG, "display start failed");
        audio::Stop();
        InfoNES_Fin();
        return false;
    }
    NesSys_SetFrameSink(&nesdisplay::SubmitFrame);
    nesaudio::Start();
    NesSys_StartPacing();

    s_monitor_running = true;
    xTaskCreatePinnedToCore(MonitorTask, "nes_input", 3072, nullptr, 3, &s_monitor_task, 0);

    const int64_t t0 = esp_timer_get_time();
    s_core_done = false;
    TaskHandle_t core_task = nullptr;
    BaseType_t created = xTaskCreatePinnedToCore(CoreTask, "nes_core", 16384, nullptr, 5,
                                                 &core_task, 1);
    if (created == pdPASS) {
        while (!s_core_done) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        ESP_LOGW(TAG, "nes_core task creation failed, running on app_main");
        InfoNES_Cycle();
    }

    const int64_t dt = esp_timer_get_time() - t0;
    const uint32_t frames = NesSys_FrameCount();
    const double fps = dt > 0 ? frames * 1000000.0 / (double)dt : 0.0;
    ESP_LOGI(TAG, "game over: %u frames / %lld ms = %.1f fps (display %u frames, %.1f ms/frame)",
             (unsigned)frames, (long long)(dt / 1000), fps,
             (unsigned)nesdisplay::RenderedFrames(), (double)nesdisplay::AverageDrawMs());

    s_monitor_running = false;
    for (int i = 0; i < 50 && s_monitor_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    NesSys_StopPacing();
    nesaudio::Stop();
    nesdisplay::Stop();
    NesSys_SetFrameSink(nullptr);
    audio::Stop();
    InfoNES_Fin();

    ESP_LOGI(TAG, "heap after unload: internal=%u largest=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

}  // namespace nesgame
