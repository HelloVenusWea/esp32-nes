#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ble_gamepad {

struct DeviceInfo {
    std::string name;
    std::string addr;
    std::string usage;
    int rssi = 0;
    uint8_t addr_type = 0;
};

/* Starts the NimBLE HID host and a background scanner. Non-blocking; the
 * first controller that advertises as gamepad/joystick is connected. */
bool Init();
bool Connected();

/* True while esp_hid_scan() is in progress. */
bool Scanning();

/* Scan results from the most recent pass (up to 8 devices). */
std::vector<DeviceInfo> Devices();

/* Ask the scanner task to run a scan as soon as possible. */
void RequestScan();

/* Manual pairing from the web UI. `addr` is "aa:bb:cc:dd:ee:ff" as printed
 * by Devices(). */
bool ConnectTo(const std::string& addr);
void Disconnect();

}  // namespace ble_gamepad
