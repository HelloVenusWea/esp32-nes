#include "nes_stream.h"

#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "input/input.h"
#include "nes_system.h"
#include "rom_store.h"

static const char* TAG = "NesStream";

namespace nesstream {
namespace {

constexpr int kNesW = 256;
constexpr int kNesH = 240;
constexpr int kFrameBytes = kNesW * kNesH;  // 61440
constexpr int kTile = 8;
constexpr int kTilesX = kNesW / kTile;      // 32
constexpr int kTilesY = kNesH / kTile;      // 30
constexpr int kTileCount = kTilesX * kTilesY;
constexpr int kPacketSize = 3 + kTileCount * (2 + 64);  // 63363
constexpr int kAudioChunk = 1920;                       // 80 ms
constexpr int kFullChunk = 4096;                        // pixels per 0x06 packet
constexpr int kPktSlots = 5;                            // ~83 ms of frame buffering

/* protocol:
 * 0x01 delta: [1] count lo, [2] count hi, then {tile u16, 64 bytes}...
 * 0x02 full:  61440 palette indices
 * 0x03 audio: [1] samples lo, [2] samples hi, then int16 LE PCM
 * 0x04 palette: 64 x RGB565 LE
 */

uint8_t* s_lut = nullptr;          // 65536 RGB565 -> palette index
uint32_t* s_tile_hash = nullptr;   // 960 tiles, last queued frame
uint8_t* s_pkt[kPktSlots] = {};    // packet buffers
uint16_t s_pkt_len[kPktSlots] = {};
QueueHandle_t s_free_q = nullptr;
QueueHandle_t s_frame_q = nullptr;

volatile bool s_active = false;
volatile bool s_stop = false;
volatile bool s_sender_running = false;
volatile bool s_need_full = true;
int s_full_pos = 0;  // next pixel offset for chunked full-frame sync
volatile uint32_t s_sent_frames = 0;
volatile uint32_t s_dropped_frames = 0;
int64_t s_last_tx_us = 0;
int s_fd = -1;
httpd_handle_t s_server = nullptr;
std::string s_name;
rom_store::RomBuffer s_rom_buf;
const uint8_t* s_rom = nullptr;
size_t s_rom_len = 0;

bool SendPacket(const uint8_t* data, size_t len) {
    if (s_server == nullptr || s_fd < 0) {
        return false;
    }
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = const_cast<uint8_t*>(data);
    frame.len = len;
    return httpd_ws_send_frame_async(s_server, s_fd, &frame) == ESP_OK;
}

void SendText(const char* text) {
    if (s_server == nullptr || s_fd < 0) {
        return;
    }
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t*)text;
    frame.len = strlen(text);
    httpd_ws_send_frame_async(s_server, s_fd, &frame);
}

bool EnsureBuffers() {
    if (s_lut != nullptr) {
        return true;
    }
    s_lut = (uint8_t*)heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tile_hash = (uint32_t*)heap_caps_malloc(kTileCount * sizeof(uint32_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_tile_hash == nullptr) {
        s_tile_hash = (uint32_t*)heap_caps_malloc(kTileCount * sizeof(uint32_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    for (int i = 0; i < kPktSlots; i++) {
        s_pkt[i] = (uint8_t*)heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    bool packets_ok = true;
    for (int i = 0; i < kPktSlots; i++) {
        packets_ok = packets_ok && s_pkt[i] != nullptr;
    }
    if (s_lut == nullptr || s_tile_hash == nullptr || !packets_ok) {
        ESP_LOGE(TAG, "stream buffers allocation failed");
        return false;
    }
    memset(s_lut, 0, 65536);
    for (int i = 0; i < 64; i++) {
        s_lut[NesPalette[i] & 0x7FFF] = (uint8_t)i;
    }
    memset(s_tile_hash, 0, kTileCount * sizeof(uint32_t));
    return true;
}

/* FNV-1a over the 64 pixels (128 bytes) of one 8x8 tile. The hash is only
 * updated when the tile is queued, so a dropped frame naturally shows up in
 * the next delta without keeping a full previous-frame buffer. */
uint32_t TileHash(const uint16_t* base) {
    using U32 = uint32_t __attribute__((may_alias));
    uint32_t h = 2166136261u;
    for (int row = 0; row < kTile; row++) {
        /* DoubleFrame is 32-byte aligned and tiles are 16-byte aligned. */
        const U32* w = (const U32*)(base + row * kNesW);
        for (int i = 0; i < 4; i++) {
            h = (h ^ w[i]) * 16777619u;
        }
    }
    return h;
}

void FrameSink(const uint16_t* frame) {
    if (!s_active || s_stop) {
        return;
    }
    uint8_t slot = 0;
    if (xQueueReceive(s_free_q, &slot, 0) != pdTRUE) {
        /* Hashes are only advanced for queued packets, so the next delta
         * still covers everything the TV missed. */
        s_dropped_frames = s_dropped_frames + 1;
        return;
    }
    uint8_t* pkt = s_pkt[slot];
    size_t p = 0;
    if (s_need_full) {
        /* 0x06 chunk: [1..2] pixel offset, [3..4] count, then indices.
         * Chunking avoids one 61 KB blocking send on a busy Wi-Fi link. */
        const int pos = s_full_pos;
        int n = kFrameBytes - pos;
        if (n > kFullChunk) {
            n = kFullChunk;
        }
        pkt[0] = 0x06;
        pkt[1] = (uint8_t)(pos & 0xFF);
        pkt[2] = (uint8_t)(pos >> 8);
        pkt[3] = (uint8_t)(n & 0xFF);
        pkt[4] = (uint8_t)(n >> 8);
        for (int i = 0; i < n; i++) {
            pkt[5 + i] = s_lut[frame[pos + i] & 0x7FFF];
        }
        p = 5 + n;
        /* A chunk is 4096 pixels = 2 tile rows = 64 whole tiles. */
        const int first_tile = pos / (kTile * kTile);
        const int n_tiles = n / (kTile * kTile);
        for (int t = 0; t < n_tiles; t++) {
            const int ti = first_tile + t;
            const int tx = ti % kTilesX;
            const int ty = ti / kTilesX;
            s_tile_hash[ti] = TileHash(frame + ty * kTile * kNesW + tx * kTile);
        }
        s_full_pos = pos + n;
        if (s_full_pos >= kFrameBytes) {
            s_full_pos = 0;
            s_need_full = false;
        }
    } else {
        /* Hash tiles in internal RAM; only changed tiles get a palette lookup. */
        pkt[0] = 0x01;
        p = 3;
        uint16_t count = 0;
        for (int ty = 0; ty < kTilesY; ty++) {
            for (int tx = 0; tx < kTilesX; tx++) {
                const int ti = ty * kTilesX + tx;
                const int off = ty * kTile * kNesW + tx * kTile;
                const uint16_t* cur = frame + off;
                const uint32_t h = TileHash(cur);
                if (h == s_tile_hash[ti]) {
                    continue;
                }
                s_tile_hash[ti] = h;
                uint8_t idx[kTile * kTile];
                for (int row = 0; row < kTile; row++) {
                    const uint16_t* src = cur + row * kNesW;
                    for (int col = 0; col < kTile; col++) {
                        idx[row * kTile + col] = s_lut[src[col] & 0x7FFF];
                    }
                }
                /* A NES tile is 2bpp: at most four palette indices. Packing
                 * the four colours plus a 16-byte bitmap is smaller than raw
                 * for busy tiles; flat tiles use RLE instead. */
                uint8_t pal[4];
                uint8_t code[kTile * kTile];
                uint8_t packed[kTile * kTile / 4] = {};
                uint8_t slot[64];
                memset(slot, 0xFF, sizeof(slot));
                int npal = 0;
                bool packable = true;
                for (int i = 0; i < kTile * kTile && packable; i++) {
                    const uint8_t c = idx[i];
                    uint8_t k = slot[c];
                    if (k == 0xFF) {
                        if (npal == 4) {
                            packable = false;
                            break;
                        }
                        k = (uint8_t)npal;
                        slot[c] = k;
                        pal[npal++] = c;
                    }
                    code[i] = k;
                    packed[i >> 2] |= (uint8_t)(k << (6 - ((i & 3) << 1)));
                }

                uint8_t payload[kTile * kTile];
                int elen = 0;
                uint16_t tag = (uint16_t)ti;
                if (!packable) {
                    memcpy(payload, idx, kTile * kTile);
                    elen = kTile * kTile;
                } else if (npal <= 2) {
                    /* Small palette: RLE is usually just a few bytes. */
                    uint8_t rle[2 + 20];
                    int rl = 0;
                    bool rle_ok = true;
                    for (int i = 0; i < kTile * kTile;) {
                        int run = 1;
                        while (i + run < kTile * kTile && idx[i + run] == idx[i]) {
                            run++;
                        }
                        if (rl + 2 > (int)sizeof(rle)) {
                            rle_ok = false;
                            break;
                        }
                        rle[rl++] = (uint8_t)run;
                        rle[rl++] = idx[i];
                        i += run;
                    }
                    if (rle_ok && rl < 2 + 4 + (int)sizeof(packed)) {
                        memcpy(payload, rle, rl);
                        elen = rl;
                        tag = (uint16_t)(ti | 0x8000);
                    } else {
                        memcpy(payload, pal, sizeof(pal));
                        memcpy(payload + sizeof(pal), packed, sizeof(packed));
                        elen = sizeof(pal) + sizeof(packed);
                        tag = (uint16_t)(ti | 0x4000);
                    }
                } else {
                    memcpy(payload, pal, sizeof(pal));
                    memcpy(payload + sizeof(pal), packed, sizeof(packed));
                    elen = sizeof(pal) + sizeof(packed);
                    tag = (uint16_t)(ti | 0x4000);
                }
                pkt[p++] = (uint8_t)(tag & 0xFF);
                pkt[p++] = (uint8_t)(tag >> 8);
                memcpy(pkt + p, payload, elen);
                p += elen;
                count++;
            }
        }
        pkt[1] = (uint8_t)(count & 0xFF);
        pkt[2] = (uint8_t)(count >> 8);
    }
    const int64_t now = esp_timer_get_time();
    if (pkt[0] == 0x01 && pkt[1] == 0 && pkt[2] == 0 &&
        now - s_last_tx_us < 250 * 1000) {
        /* Static screen: no need to wake Wi-Fi at 60 Hz just for a heartbeat. */
        xQueueSend(s_free_q, &slot, 0);
        return;
    }
    s_last_tx_us = now;
    s_pkt_len[slot] = (uint16_t)p;
    if (xQueueSend(s_frame_q, &slot, 0) != pdTRUE) {
        s_dropped_frames = s_dropped_frames + 1;
        s_need_full = true;
        s_full_pos = 0;
        memset(s_tile_hash, 0, kTileCount * sizeof(uint32_t));
        xQueueSend(s_free_q, &slot, 0);
        return;
    }
}

void SenderTask(void* arg) {
    (void)arg;
    s_sender_running = true;
    int64_t last_pad = 0;
    int16_t audio[kAudioChunk];
    uint8_t audio_pkt[3 + kAudioChunk * 2];

    while (!s_stop) {
        /* Audio first: a blocked video send must not starve the APU ring. */
        for (int a = 0; a < 2 && NesSys_AudioAvailable() >= (uint32_t)kAudioChunk; a++) {
            const uint32_t n = NesSys_AudioRead(audio, kAudioChunk);
            if (n > 0) {
                audio_pkt[0] = 0x03;
                audio_pkt[1] = (uint8_t)(n & 0xFF);
                audio_pkt[2] = (uint8_t)(n >> 8);
                memcpy(audio_pkt + 3, audio, n * sizeof(int16_t));
                if (!SendPacket(audio_pkt, 3 + n * sizeof(int16_t))) {
                    s_stop = true;
                    NesSys_RequestStop();
                    break;
                }
            }
        }
        if (s_stop) {
            break;
        }

        uint8_t slot = 0;
        if (xQueueReceive(s_frame_q, &slot, pdMS_TO_TICKS(5)) == pdTRUE) {
            const bool ok = SendPacket(s_pkt[slot], s_pkt_len[slot]);
            xQueueSend(s_free_q, &slot, 0);
            if (!ok) {
                ESP_LOGW(TAG, "stream send failed");
                s_stop = true;
                NesSys_RequestStop();
                break;
            }
            s_sent_frames = s_sent_frames + 1;
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_pad >= 20 * 1000) {
            last_pad = now;
            input::ScanResult scan = input::Scan();
            NesSys_SetPad(scan.pad);
            if (scan.boot_long) {
                ESP_LOGI(TAG, "BOOT long press, stopping stream");
                Stop();
            }
        }
    }

    s_fd = -1;
    s_sender_running = false;
    vTaskDelete(nullptr);
}

void EmuTask(void* arg) {
    (void)arg;
    s_active = true;
    s_need_full = true;
    NesSys_ResetState();
    NesSys_SetRom(s_rom, s_rom_len);
    InfoNES_Init();
    if (InfoNES_Load("") != 0) {
        ESP_LOGE(TAG, "stream ROM load failed");
        s_stop = true;
        s_active = false;
        vTaskDelete(nullptr);
        return;
    }
    NesSys_SetFrameSink(&FrameSink);
    NesSys_StartPacing();
    const int64_t t0 = esp_timer_get_time();
    InfoNES_Cycle();
    const int64_t dt = esp_timer_get_time() - t0;
    const uint32_t frames = NesSys_FrameCount();
    NesSys_StopPacing();
    NesSys_SetFrameSink(nullptr);
    InfoNES_Fin();
    s_active = false;
    s_stop = true;
    s_rom_buf.Reset();
    s_rom = nullptr;
    s_rom_len = 0;
    ESP_LOGI(TAG, "stream finished: %u frames / %lld ms, sent %u, dropped %u, audio overruns %u",
             (unsigned)frames, (long long)(dt / 1000), (unsigned)s_sent_frames,
             (unsigned)s_dropped_frames, (unsigned)NesSys_AudioOverruns());
    vTaskDelete(nullptr);
}

}  // namespace

bool Start(const std::string& rom_name, int ws_fd, httpd_handle_t server) {
    s_fd = ws_fd;
    s_server = server;
    if (s_active || s_sender_running) {
        /* A page reload can race the previous session's shutdown. Give the
         * old emulator/sender a moment to exit instead of failing outright. */
        s_stop = true;
        NesSys_RequestStop();
        for (int i = 0; i < 70 && (s_active || s_sender_running); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_active || s_sender_running) {
            SendText("ERR busy");
            s_fd = -1;
            return false;
        }
    }
    if (!EnsureBuffers()) {
        SendText("ERR no memory");
        s_fd = -1;
        return false;
    }

    s_name = rom_name;
    s_stop = false;
    s_need_full = true;
    s_full_pos = 0;
    memset(s_tile_hash, 0, kTileCount * sizeof(uint32_t));
    s_sent_frames = 0;
    s_dropped_frames = 0;
    s_last_tx_us = 0;
    NesSys_ResetState();

    const std::string builtin = rom_store::BuiltinEntry().name;
    if (!builtin.empty() && rom_name == builtin) {
        s_rom = rom_store::BuiltinRom(&s_rom_len);
    } else {
        if (!rom_store::Read(rom_name, s_rom_buf)) {
            SendText("ERR ROM not found");
            s_fd = -1;
            return false;
        }
        s_rom = s_rom_buf.data;
        s_rom_len = s_rom_buf.size;
    }
    if (s_rom == nullptr || s_rom_len < 16 || memcmp(s_rom, "NES\x1a", 4) != 0) {
        SendText("ERR invalid ROM");
        s_rom_buf.Reset();
        s_rom = nullptr;
        s_rom_len = 0;
        s_fd = -1;
        return false;
    }

    if (s_free_q != nullptr) {
        vQueueDelete(s_free_q);
        s_free_q = nullptr;
    }
    if (s_frame_q != nullptr) {
        vQueueDelete(s_frame_q);
        s_frame_q = nullptr;
    }
    s_free_q = xQueueCreate(kPktSlots, sizeof(uint8_t));
    s_frame_q = xQueueCreate(kPktSlots, sizeof(uint8_t));
    if (s_free_q == nullptr || s_frame_q == nullptr) {
        SendText("ERR queue");
        s_fd = -1;
        return false;
    }
    for (uint8_t i = 0; i < kPktSlots; i++) {
        xQueueSend(s_free_q, &i, 0);
    }

    uint8_t palette[1 + 64 * 2];
    palette[0] = 0x04;
    for (int i = 0; i < 64; i++) {
        palette[1 + i * 2] = (uint8_t)(NesPalette[i] & 0xFF);
        palette[2 + i * 2] = (uint8_t)(NesPalette[i] >> 8);
    }
    if (!SendPacket(palette, sizeof(palette))) {
        SendText("ERR socket");
        s_fd = -1;
        return false;
    }

    s_active = true;
    if (xTaskCreatePinnedToCore(EmuTask, "nes_stream", 16384, nullptr, 5, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "emu task creation failed");
        s_active = false;
        s_fd = -1;
        return false;
    }
    if (xTaskCreatePinnedToCore(SenderTask, "nes_sender", 12288, nullptr, 4, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "sender task creation failed");
        Stop();
        return false;
    }
    ESP_LOGI(TAG, "stream started: %s", rom_name.c_str());
    return true;
}

void Stop() {
    if (!s_active && !s_sender_running) {
        return;
    }
    ESP_LOGI(TAG, "stream stop requested");
    s_stop = true;
    NesSys_RequestStop();
}

bool Active() {
    return s_active;
}

std::string CurrentRom() {
    return s_name;
}

}  // namespace nesstream
