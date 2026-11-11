#pragma once

#include <cstdint>

namespace input {

/* NES pad bits (same layout as InfoNES PadState) */
constexpr uint8_t kA = 1 << 0;
constexpr uint8_t kB = 1 << 1;
constexpr uint8_t kSelect = 1 << 2;
constexpr uint8_t kStart = 1 << 3;
constexpr uint8_t kUp = 1 << 4;
constexpr uint8_t kDown = 1 << 5;
constexpr uint8_t kLeft = 1 << 6;
constexpr uint8_t kRight = 1 << 7;

struct ScanResult {
    uint8_t pad = 0;        // current pad state
    uint8_t pressed = 0;    // bits that went 0 -> 1 since the last scan
    uint8_t released = 0;   // bits that went 1 -> 0 since the last scan
    bool boot_short = false;
    bool boot_long = false;
};

void Init();

/* Poll at 10-50 ms intervals from the menu or from the game monitor task. */
ScanResult Scan();

uint8_t PadMask();

/* Called from the BLE HID callback. */
void SetPad(uint8_t mask);

/* For the menu status line; empty when no gamepad is connected. */
const char* PadName();
void SetPadName(const char* name);

}  // namespace input
