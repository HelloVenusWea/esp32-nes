#include "menu_ui.h"

#include <cstdio>
#include <cstring>

#include "board_pins.h"
#include "panel_display.h"

namespace menu_ui {
namespace {

constexpr int kW = board::kScreenWidth;
constexpr int kH = board::kScreenHeight;
constexpr int kHeaderH = 36;
constexpr int kFooterH = 40;
constexpr int kRowH = 20;
constexpr int kVisibleRows = (kH - kHeaderH - kFooterH) / kRowH;  // 8

const uint16_t kBg = display::Rgb565(8, 10, 18);
const uint16_t kHeader = display::Rgb565(13, 19, 32);
const uint16_t kFooter = display::Rgb565(11, 15, 25);
const uint16_t kText = display::Rgb565(232, 237, 247);
const uint16_t kMuted = display::Rgb565(139, 151, 173);
const uint16_t kAccent = display::Rgb565(90, 209, 255);
const uint16_t kGold = display::Rgb565(255, 209, 90);
const uint16_t kSelBg = display::Rgb565(24, 52, 78);
const uint16_t kDim = display::Rgb565(95, 106, 128);

bool HasNonAscii(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0x80) {
            return true;
        }
    }
    return false;
}

void DrawHeader(const std::string& pad_name, const std::string& ssid, const std::string& ip,
                int selected, int total) {
    display::FillRect(0, 0, kW, kHeaderH, kHeader);
    display::DrawText(8, 6, "ESP32-NES", kAccent, kHeader, 2);

    char right[64];
    const char* pad = pad_name.empty() ? "none" : pad_name.c_str();
    snprintf(right, sizeof(right), "PAD: %s", pad);
    int pad_chars = (int)strlen(right);
    int pad_x = kW - 8 - pad_chars * 8;
    if (pad_x < 160) {
        pad_x = 160;
    }
    display::DrawText(pad_x, 6, right, pad_name.empty() ? kMuted : kGold, kHeader, 1,
                      (kW - 8 - pad_x) / 8);

    char net[80];
    snprintf(net, sizeof(net), "%s  %s", ssid.empty() ? "AP: -" : ssid.c_str(),
             ip.empty() ? "" : ip.c_str());
    display::DrawText(8, 23, net, kDim, kHeader, 1, 27);

    if (total > 0) {
        char scroll[24];
        snprintf(scroll, sizeof(scroll), "%d/%d", selected + 1, total);
        display::DrawText(kW - 8 - (int)strlen(scroll) * 8, 23, scroll, kDim, kHeader, 1, 10);
    }
}

void DrawFooter() {
    display::FillRect(0, kH - kFooterH, kW, kFooterH, kFooter);
    display::DrawText(8, kH - kFooterH + 5, "UP/DN select  A/START play  B refresh", kMuted,
                      kFooter, 1, 37);
    display::DrawText(8, kH - kFooterH + 21, "BOOT next  hold BOOT play/quit", kDim,
                      kFooter, 1, 32);
}

}  // namespace

void Draw(const std::vector<rom_store::RomEntry>& roms, int selected,
          const std::string& pad_name, const std::string& ssid, const std::string& ip) {
    display::Clear(kBg);
    DrawHeader(pad_name, ssid, ip, selected, (int)roms.size());

    const int n = (int)roms.size();
    if (n == 0) {
        display::DrawText(24, 96, "No ROMs on this device.", kGold, kBg, 2);
        char line[64];
        if (!ip.empty()) {
            snprintf(line, sizeof(line), "Open http://%s/ to upload", ip.c_str());
        } else {
            snprintf(line, sizeof(line), "Join %s and upload ROMs", ssid.c_str());
        }
        display::DrawText(24, 124, line, kMuted, kBg, 1, 38);
        DrawFooter();
        display::FlushAll();
        return;
    }

    int first = 0;
    if (selected >= kVisibleRows) {
        first = selected - kVisibleRows + 1;
    }
    if (first > n - kVisibleRows) {
        first = n - kVisibleRows;
    }
    if (first < 0) {
        first = 0;
    }

    for (int row = 0; row < kVisibleRows; row++) {
        const int idx = first + row;
        if (idx >= n) {
            break;
        }
        const int y = kHeaderH + row * kRowH;
        const bool sel = idx == selected;
        const uint16_t bg = sel ? kSelBg : kBg;
        if (sel) {
            display::FillRect(0, y, kW, kRowH, kSelBg);
        }
        char index[16];
        snprintf(index, sizeof(index), "%c %02d", sel ? '>' : ' ', idx + 1);
        const uint16_t row_fg = sel ? kText : (roms[idx].builtin ? kAccent : kText);
        display::DrawText(8, y + 6, index, row_fg, bg, 1, 4);

        const int name_x = 52;
        const int tag_w = roms[idx].builtin ? 56 : 0;
        const int max_px = kW - 8 - tag_w - name_x;
        if (HasNonAscii(roms[idx].name)) {
            display::DrawTextCjk(name_x, y + 2, roms[idx].name.c_str(), row_fg, bg, 1, max_px);
        } else {
            display::DrawText(name_x, y + 6, roms[idx].name.c_str(), row_fg, bg, 1, max_px / 8);
        }
        if (roms[idx].builtin) {
            display::DrawText(kW - 8 - 7 * 8, y + 6, "BUILTIN", kDim, bg, 1, 7);
        }
    }

    if (n > kVisibleRows) {
        /* counter already in the header */
    }

    DrawFooter();
    display::FlushAll();
}

void DrawMessage(const char* title, const char* line1, const char* line2) {
    display::Clear(kBg);
    display::DrawText(16, 16, "ESP32-NES", kAccent, kBg, 2);
    display::DrawText(16, 64, title ? title : "", kGold, kBg, 2, 18);
    if (line1 != nullptr) {
        display::DrawText(16, 100, line1, kMuted, kBg, 1, 38);
    }
    if (line2 != nullptr) {
        display::DrawText(16, 118, line2, kMuted, kBg, 1, 38);
    }
    display::FlushAll();
}

}  // namespace menu_ui
