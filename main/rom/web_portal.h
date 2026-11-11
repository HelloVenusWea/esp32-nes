#pragma once

#include <cstdint>
#include <string>

namespace web {

/* SoftAP + HTTP server for ROM upload and management. */
bool Start();

/* Called around a game: stopping Wi-Fi frees airtime and internal RAM for
 * BLE input and emulation. Resume() brings the AP back up. */
void Suspend();
void Resume();

std::string Ip();
std::string Ssid();

/* Incremented after every upload/delete so the menu can refresh its list. */
uint32_t Generation();

/* Set by POST /api/play; the menu consumes it and starts the ROM. */
std::string TakePlayRequest();

/* True while at least one /ws/pad client is connected. The menu uses this to
 * avoid starting a local game (which would stop Wi-Fi and kill the stream). */
bool TvActive();

}  // namespace web
