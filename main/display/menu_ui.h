#pragma once

#include <string>
#include <vector>

#include "rom_store.h"

namespace menu_ui {

void Draw(const std::vector<rom_store::RomEntry>& roms, int selected,
          const std::string& pad_name, const std::string& ssid, const std::string& ip);

void DrawMessage(const char* title, const char* line1, const char* line2);

}  // namespace menu_ui
