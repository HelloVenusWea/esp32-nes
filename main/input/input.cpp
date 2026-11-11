#include "input.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board_init.h"
#include "board_pins.h"

static const char* TAG = "Input";

namespace input {
namespace {

constexpr int64_t kBootLongUs = 800 * 1000;
constexpr int64_t kBootDebounceUs = 20 * 1000;

volatile uint8_t s_pad = 0;
uint8_t s_last_pad = 0;

bool s_boot_raw = false;
bool s_boot_stable = false;
int64_t s_boot_change_us = 0;
int64_t s_boot_press_us = 0;
bool s_boot_long_fired = false;

char s_pad_name[64] = {};

void UpdateBoot(ScanResult* out) {
    const int64_t now = esp_timer_get_time();
    const bool raw = board::BootButtonDown();
    if (raw != s_boot_raw) {
        s_boot_raw = raw;
        s_boot_change_us = now;
    }
    if (now - s_boot_change_us >= kBootDebounceUs && raw != s_boot_stable) {
        s_boot_stable = raw;
        if (raw) {
            s_boot_press_us = now;
            s_boot_long_fired = false;
        } else if (!s_boot_long_fired && now - s_boot_press_us < kBootLongUs) {
            out->boot_short = true;
        }
    }
    if (s_boot_stable && !s_boot_long_fired && now - s_boot_press_us >= kBootLongUs) {
        s_boot_long_fired = true;
        out->boot_long = true;
    }
}

}  // namespace

void Init() {
    s_pad = 0;
    s_last_pad = 0;
    s_boot_raw = board::BootButtonDown();
    s_boot_stable = s_boot_raw;
    s_boot_change_us = esp_timer_get_time();
    s_boot_press_us = s_boot_change_us;
    s_boot_long_fired = false;
}

ScanResult Scan() {
    ScanResult out;
    UpdateBoot(&out);
    out.pad = s_pad;
    out.pressed = (uint8_t)(s_pad & ~s_last_pad);
    out.released = (uint8_t)(~s_pad & s_last_pad);
    s_last_pad = s_pad;
    return out;
}

uint8_t PadMask() {
    return s_pad;
}

void SetPad(uint8_t mask) {
    if (mask != s_pad) {
        ESP_LOGI(TAG, "pad 0x%02x", mask);
    }
    s_pad = mask;
}

const char* PadName() {
    return s_pad_name;
}

void SetPadName(const char* name) {
    if (name == nullptr) {
        s_pad_name[0] = 0;
        return;
    }
    strncpy(s_pad_name, name, sizeof(s_pad_name) - 1);
    s_pad_name[sizeof(s_pad_name) - 1] = 0;
    ESP_LOGI(TAG, "pad: %s", s_pad_name);
}

}  // namespace input
