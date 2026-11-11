#include <cstring>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "audio_out.h"
#include "board_init.h"
#include "board_pins.h"
#include "display/menu_ui.h"
#include "display/panel_display.h"
#include "input/input.h"
#include "input/ble_gamepad.h"
#include "nes/nes_game.h"
#include "nes/nes_stream.h"
#include "rom/rom_store.h"
#include "rom/web_portal.h"

static const char* TAG = "App";

namespace {

bool StartGame(const rom_store::RomEntry& entry) {
    menu_ui::DrawMessage("LOADING", entry.name.c_str(), nullptr);

    /* Wi-Fi off during gameplay: less 2.4 GHz contention for the BLE pad and
     * roughly 40 KB of internal RAM back for the emulator. */
    web::Suspend();
    bool ok = false;
    if (entry.builtin) {
        size_t len = 0;
        const uint8_t* rom = rom_store::BuiltinRom(&len);
        ok = nesgame::Run(rom, len, entry.name.c_str());
    } else {
        rom_store::RomBuffer buf;
        if (rom_store::Read(entry.name, buf)) {
            ok = nesgame::Run(buf.data, buf.size, entry.name.c_str());
        }
    }
    web::Resume();
    return ok;
}

}  // namespace

#if NES_HEADLESS
/* Bare ESP32-S3 build: no ST7789/ES8388/XL9555. Wi-Fi, BLE gamepad,
 * ROM storage and the browser/TV streaming endpoints still run. */
extern "C" void app_main() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    input::Init();
    if (!rom_store::Init()) {
        ESP_LOGE(TAG, "ROM storage unavailable");
    }
    if (!web::Start()) {
        ESP_LOGE(TAG, "web portal unavailable");
    }
    if (!ble_gamepad::Init()) {
        ESP_LOGW(TAG, "BLE gamepad host unavailable");
    }

    ESP_LOGI(TAG, "HEADLESS TV stream mode (no local display/audio)");
    ESP_LOGI(TAG, "network: SSID \"%s\", open http://%s/", web::Ssid().c_str(),
             web::Ip().c_str());
    ESP_LOGI(TAG, "use /stream or the Android TV APK; local /api/play is ignored");

    while (true) {
        std::string request = web::TakePlayRequest();
        if (!request.empty()) {
            ESP_LOGW(TAG, "local play ignored in headless mode: %s", request.c_str());
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#else
extern "C" void app_main() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(board::Init());
    if (!display::InitCanvas(board::Get().panel, board::kScreenWidth, board::kScreenHeight)) {
        ESP_LOGE(TAG, "display canvas init failed");
        return;
    }
    menu_ui::DrawMessage("BOOTING", "ESP32-S3 NES console", "starting Wi-Fi...");
    input::Init();

    if (!audio::Init(board::Get().i2c)) {
        ESP_LOGE(TAG, "audio init failed");
    }
    if (!rom_store::Init()) {
        ESP_LOGE(TAG, "ROM storage unavailable");
    }
    if (!web::Start()) {
        ESP_LOGW(TAG, "web portal unavailable");
    }
    if (!ble_gamepad::Init()) {
        ESP_LOGW(TAG, "BLE gamepad host unavailable");
    }

    ESP_LOGI(TAG, "heap: internal free=%u largest=%u, PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    std::vector<rom_store::RomEntry> roms = rom_store::List();
    int selected = 0;
    uint32_t last_generation = web::Generation();
    bool redraw = true;
    int64_t up_repeat = 0;
    int64_t down_repeat = 0;
    int64_t last_status_check = 0;
    std::string last_pad_name = input::PadName();
    bool tv_shown = false;

    while (true) {
        if (web::Generation() != last_generation) {
            last_generation = web::Generation();
            roms = rom_store::List();
            if (selected >= (int)roms.size()) {
                selected = (int)roms.size() - 1;
            }
            if (selected < 0) {
                selected = 0;
            }
            redraw = true;
        }

        const bool tv_now = web::TvActive() || nesstream::Active();
        if (tv_now != tv_shown) {
            tv_shown = tv_now;
            if (tv_now) {
                if (nesstream::Active()) {
                    menu_ui::DrawMessage("TV STREAM", nesstream::CurrentRom().c_str(),
                                         "Close the TV app to return");
                } else {
                    menu_ui::DrawMessage("TV MODE", "Playing on browser /tv",
                                         "Close the TV page to return");
                }
            } else {
                redraw = true;
            }
        }
        if (tv_now) {
            /* Keep Wi-Fi running and do not start a local game. For a stream
             * session the emulator runs headless on core 1; for /tv the browser
             * runs jsnes and only needs the pad stream. */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        input::ScanResult scan = input::Scan();
        const int64_t now = esp_timer_get_time();
        bool play = false;

        /* Remote start from the web page. */
        std::string request = web::TakePlayRequest();
        if (!request.empty()) {
            for (size_t i = 0; i < roms.size(); i++) {
                if (roms[i].name == request) {
                    selected = (int)i;
                    play = true;
                    redraw = true;
                    break;
                }
            }
            if (!play) {
                ESP_LOGW(TAG, "web requested unknown ROM: %s", request.c_str());
            } else {
                /* Let the HTTP response reach the browser before Wi-Fi is
                 * stopped for game mode. */
                vTaskDelay(pdMS_TO_TICKS(600));
            }
        }

        if (!roms.empty()) {
            if (scan.boot_short) {
                selected = (selected + 1) % (int)roms.size();
                redraw = true;
            }
            if (scan.pressed & input::kUp) {
                selected = (selected - 1 + (int)roms.size()) % (int)roms.size();
                up_repeat = now + 400 * 1000;
                redraw = true;
            } else if ((scan.pad & input::kUp) && now >= up_repeat && up_repeat != 0) {
                selected = (selected - 1 + (int)roms.size()) % (int)roms.size();
                up_repeat = now + 120 * 1000;
                redraw = true;
            }
            if (scan.pressed & input::kDown) {
                selected = (selected + 1) % (int)roms.size();
                down_repeat = now + 400 * 1000;
                redraw = true;
            } else if ((scan.pad & input::kDown) && now >= down_repeat && down_repeat != 0) {
                selected = (selected + 1) % (int)roms.size();
                down_repeat = now + 120 * 1000;
                redraw = true;
            }
            if (scan.pressed & (input::kA | input::kStart)) {
                play = true;
            }
            if (scan.pressed & input::kB) {
                roms = rom_store::List();
                if (selected >= (int)roms.size()) {
                    selected = (int)roms.size() - 1;
                }
                if (selected < 0) {
                    selected = 0;
                }
                redraw = true;
            }
            if (scan.boot_long) {
                play = true;
            }
        }

        if (play && !roms.empty()) {
            const rom_store::RomEntry entry = roms[selected];
            bool ok = StartGame(entry);
            if (!ok) {
                menu_ui::DrawMessage("FAILED", "Could not start:", entry.name.c_str());
                vTaskDelay(pdMS_TO_TICKS(1800));
            }
            roms = rom_store::List();
            if (selected >= (int)roms.size()) {
                selected = (int)roms.size() - 1;
            }
            if (selected < 0) {
                selected = 0;
            }
            redraw = true;
        }

        if (redraw) {
            menu_ui::Draw(roms, selected, input::PadName(), web::Ssid(), web::Ip());
            redraw = false;
        } else if (now - last_status_check > 1000 * 1000) {
            /* Refresh the header when a BLE pad connects/disconnects. */
            if (last_pad_name != input::PadName()) {
                last_pad_name = input::PadName();
                menu_ui::Draw(roms, selected, input::PadName(), web::Ssid(), web::Ip());
            }
            last_status_check = now;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

#endif  // NES_HEADLESS
