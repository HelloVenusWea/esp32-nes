#pragma once

#include <cstdint>

namespace ble_raw_hid {

/* Minimal NimBLE GATT HID client for gamepads that do not expose a HID
 * Report Map (e.g. ShanWan BM-835). It discovers the HID service, subscribes
 * to the Report characteristics and feeds raw notifications to the input
 * layer. */
bool Connect(const uint8_t* bda, uint8_t addr_type);
bool Connected();
bool Connecting();
void Disconnect();

}  // namespace ble_raw_hid
