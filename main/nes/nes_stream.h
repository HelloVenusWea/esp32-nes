#pragma once

#include <cstdint>
#include <string>

#include <esp_http_server.h>

namespace nesstream {

/* TV-only headless session: runs InfoNES on core 1, encodes indexed 8x8 tile
 * deltas + 24 kHz PCM, and pushes them over /ws/stream. Does not touch the
 * local display/audio or stop Wi-Fi. */
bool Start(const std::string& rom_name, int ws_fd, httpd_handle_t server);
void Stop();
bool Active();
std::string CurrentRom();

}  // namespace nesstream
