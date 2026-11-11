#include "nes_system.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"
#include "K6502.h"

static const char* TAG = "NesSys";

/* Standard NES palette (RGB565, native order). WorkFrame also stores the
 * sprite-priority flag in bit 15, so the display path must clear that bit
 * before swapping bytes for ST7789; the palette itself cannot be pre-swapped. */
WORD NesPalette[64] = {
    0x39ce, 0x1071, 0x0015, 0x2013, 0x440e, 0x5402, 0x5000, 0x3c20,
    0x20a0, 0x0100, 0x0140, 0x00e2, 0x0ceb, 0x0000, 0x0000, 0x0000,
    0x5ef7, 0x01dd, 0x10fd, 0x401e, 0x5c17, 0x700b, 0x6ca0, 0x6521,
    0x45c0, 0x0240, 0x02a0, 0x0247, 0x0211, 0x0000, 0x0000, 0x0000,
    0x7fff, 0x1eff, 0x2e5f, 0x223f, 0x79ff, 0x7dd6, 0x7dcc, 0x7e67,
    0x7ae7, 0x4342, 0x2769, 0x2ff3, 0x03bb, 0x0000, 0x0000, 0x0000,
    0x7fff, 0x579f, 0x635f, 0x6b3f, 0x7f1f, 0x7f1b, 0x7ef6, 0x7f75,
    0x7f94, 0x73f4, 0x57d7, 0x5bf9, 0x4ffe, 0x0000, 0x0000, 0x0000,
};

namespace {

const uint8_t* s_rom = nullptr;
size_t s_rom_len = 0;

/* bit0=A bit1=B bit2=Select bit3=Start
 * bit4=Up bit5=Down bit6=Left bit7=Right */
volatile uint8_t s_pad1 = 0;

volatile uint32_t s_frame_count = 0;
volatile uint32_t s_frame_limit = 0;  // 0 = unlimited
volatile bool s_quit = false;
volatile bool s_debug = false;
bool s_thumb_done = false;

typedef void (*FrameSink)(const uint16_t* frame);
FrameSink s_frame_sink = nullptr;

/* NTSC 60.0988 Hz -> 16639 us. FreeRTOS ticks are 1 ms, so use esp_timer. */
constexpr int64_t kFrameIntervalUs = 16639;
SemaphoreHandle_t s_vsync_sem = nullptr;
esp_timer_handle_t s_vsync_timer = nullptr;

/* APU output ring: 24 kHz mono int16 in PSRAM (2.7 s). */
constexpr int kAudioRingLen = 65536;
int16_t* s_audio_ring = nullptr;
volatile uint32_t s_audio_w = 0;
volatile uint32_t s_audio_r = 0;
volatile uint32_t s_audio_overruns = 0;
volatile float s_resample_phase = 0.0f;
int16_t s_prev_sample = 0;

/* Small ROMs go to internal RAM: 6502 opcode fetch from PSRAM was the main
 * reason the integrated firmware only reached ~50 fps. */
constexpr size_t kInternalRomLimit = 96 * 1024;

void VsTimerCb(void* arg) {
    (void)arg;
    if (s_vsync_sem != nullptr) {
        xSemaphoreGive(s_vsync_sem);
    }
}

int16_t* RingInit() {
    if (s_audio_ring == nullptr) {
        s_audio_ring = (int16_t*)heap_caps_malloc(kAudioRingLen * sizeof(int16_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_audio_ring;
}

void* AllocRom(size_t size, bool prefer_internal) {
    void* p = nullptr;
    if (prefer_internal) {
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p != nullptr) {
            return p;
        }
    }
    p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = malloc(size);
    }
    return p;
}

void DumpThumbnail(const uint16_t* f) {
    static const char kChars[] = " .:-=+*#%@";
    constexpr int kCols = 64;
    constexpr int kRows = 24;
    ESP_LOGI(TAG, "frame 120 brightness thumbnail (%dx%d):", kCols, kRows);
    for (int ry = 0; ry < kRows; ry++) {
        char line[kCols + 1];
        for (int rx = 0; rx < kCols; rx++) {
            uint32_t sum = 0;
            for (int y = ry * 10; y < (ry + 1) * 10; y++) {
                for (int x = rx * 4; x < (rx + 1) * 4; x++) {
                    uint16_t v = f[y * NES_DISP_WIDTH + x] & 0x7FFF;
                    sum += ((v >> 11) & 0x1F) * 2 + ((v >> 5) & 0x3F) + (v & 0x1F) * 2;
                }
            }
            int lum = (int)(sum / 40);
            int idx = lum * 9 / 188;
            if (idx > 9) idx = 9;
            line[rx] = kChars[idx];
        }
        line[kCols] = 0;
        ESP_LOGI(TAG, "|%s|", line);
    }
}

}  // namespace

/* ---- API for player / display / audio ---- */

void NesSys_SetRom(const uint8_t* data, size_t len) {
    s_rom = data;
    s_rom_len = len;
}

void NesSys_SetPad(uint8_t pad_mask) {
    s_pad1 = pad_mask;
}

uint32_t NesSys_AudioRead(int16_t* out, uint32_t max_samples) {
    if (s_audio_ring == nullptr) {
        return 0;
    }
    uint32_t avail = s_audio_w - s_audio_r;
    uint32_t n = avail < max_samples ? avail : max_samples;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_audio_ring[(s_audio_r + i) % kAudioRingLen];
    }
    s_audio_r += n;
    return n;
}

uint32_t NesSys_AudioAvailable() {
    return s_audio_ring ? (s_audio_w - s_audio_r) : 0;
}

uint32_t NesSys_AudioOverruns() {
    return s_audio_overruns;
}

void NesSys_ResetState() {
    s_frame_count = 0;
    s_frame_limit = 0;
    s_quit = false;
    s_audio_w = 0;
    s_audio_r = 0;
    s_audio_overruns = 0;
    s_resample_phase = 0.0f;
    s_prev_sample = 0;
}

void NesSys_RequestStop() {
    s_quit = true;
}

void NesSys_StartPacing() {
    if (s_vsync_sem == nullptr) {
        s_vsync_sem = xSemaphoreCreateBinary();
    }
    if (s_vsync_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &VsTimerCb;
        args.name = "nes_vsync";
        esp_timer_create(&args, &s_vsync_timer);
    }
    if (s_vsync_timer != nullptr) {
        esp_timer_stop(s_vsync_timer);
        esp_timer_start_periodic(s_vsync_timer, kFrameIntervalUs);
    }
}

void NesSys_StopPacing() {
    if (s_vsync_timer != nullptr) {
        esp_timer_stop(s_vsync_timer);
    }
}

void NesSys_SetFrameSink(void (*sink)(const uint16_t* frame)) {
    s_frame_sink = sink;
}

void NesSys_SetFrameLimit(uint32_t limit) {
    s_frame_limit = limit;
}

void NesSys_SetDebug(bool enabled) {
    s_debug = enabled;
    s_thumb_done = false;
}

uint32_t NesSys_FrameCount() {
    return s_frame_count;
}

/* ---- InfoNES platform hooks ---- */

int InfoNES_Menu() {
    return 0;
}

int InfoNES_ReadRom(const char* pszFileName) {
    (void)pszFileName;
    if (s_rom == nullptr || s_rom_len < 16) {
        ESP_LOGE(TAG, "empty ROM buffer");
        return -1;
    }
    const uint8_t* p = s_rom;
    size_t remain = s_rom_len;

    memcpy(&NesHeader, p, sizeof(NesHeader));
    p += sizeof(NesHeader);
    remain -= sizeof(NesHeader);
    if (memcmp(NesHeader.byID, "NES\x1a", 4) != 0) {
        ESP_LOGE(TAG, "not a valid .nes file");
        return -1;
    }

    memset(SRAM, 0, SRAM_SIZE);
    if (NesHeader.byInfo1 & 4) {
        if (remain < 512) {
            return -1;
        }
        memcpy(&SRAM[0x1000], p, 512);
        p += 512;
        remain -= 512;
    }

    const size_t rom_size = (size_t)NesHeader.byRomSize * 0x4000;
    if (rom_size > remain) {
        ESP_LOGE(TAG, "truncated PRG: need %u, have %u", (unsigned)rom_size, (unsigned)remain);
        return -1;
    }
    const size_t vrom_size = (size_t)NesHeader.byVRomSize * 0x2000;
    if (vrom_size > remain - rom_size) {
        ESP_LOGE(TAG, "truncated CHR");
        return -1;
    }
    const bool prefer_internal = (rom_size + vrom_size) <= kInternalRomLimit;

    ROM = (BYTE*)AllocRom(rom_size, prefer_internal);
    if (ROM == nullptr) {
        ESP_LOGE(TAG, "PRG allocation failed (%u bytes)", (unsigned)rom_size);
        return -1;
    }
    memcpy(ROM, p, rom_size);
    p += rom_size;
    remain -= rom_size;

    if (NesHeader.byVRomSize > 0) {
        VROM = (BYTE*)AllocRom(vrom_size, prefer_internal);
        if (VROM == nullptr) {
            ESP_LOGE(TAG, "CHR allocation failed (%u bytes)", (unsigned)vrom_size);
            return -1;
        }
        memcpy(VROM, p, vrom_size);
    }
    ESP_LOGI(TAG, "ROM ready: %u PRG + %u CHR banks, mapper %d, %s RAM",
             (unsigned)NesHeader.byRomSize, (unsigned)NesHeader.byVRomSize,
             (int)((NesHeader.byInfo1 >> 4) | (NesHeader.byInfo2 & 0xf0)),
             prefer_internal ? "internal" : "PSRAM");
    return 0;
}

void InfoNES_ReleaseRom() {
    if (ROM) {
        free(ROM);
        ROM = nullptr;
    }
    if (VROM) {
        free(VROM);
        VROM = nullptr;
    }
}

void InfoNES_LoadFrame() {
    s_frame_count = s_frame_count + 1;
    if (s_debug && s_frame_count == 120 && !s_thumb_done) {
        s_thumb_done = true;
        DumpThumbnail((const uint16_t*)WorkFrame);
    }
    if (s_debug && s_frame_count % 300 == 0) {
        ESP_LOGI(TAG, "frame %u, audio available %u", (unsigned)s_frame_count,
                 (unsigned)NesSys_AudioAvailable());
    }
    if (s_frame_sink != nullptr) {
        s_frame_sink((const uint16_t*)WorkFrame);
    }
    if (s_frame_limit != 0 && s_frame_count >= s_frame_limit) {
        s_quit = true;
    }
    if (s_vsync_sem != nullptr) {
        xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(100));
    }
    /* If the emulator is behind schedule the vsync binary semaphore is always
     * ready, so this task never blocks. Yield once in a while to keep IDLE0
     * alive and avoid the task watchdog. */
    static uint32_t yield_counter = 0;
    if ((++yield_counter % 30) == 0) {
        vTaskDelay(1);
    }
}

void InfoNES_PadState(DWORD* pdwPad1, DWORD* pdwPad2, DWORD* pdwSystem) {
    *pdwPad1 = s_pad1;
    *pdwPad2 = 0;
    *pdwSystem = s_quit ? PAD_SYS_QUIT : 0;
}

void* InfoNES_MemoryCopy(void* dest, const void* src, int count) {
    return memcpy(dest, src, count);
}

void* InfoNES_MemorySet(void* dest, int c, int count) {
    return memset(dest, c, count);
}

void InfoNES_DebugPrint(char* pszMsg) {
    ESP_LOGI(TAG, "%s", pszMsg ? pszMsg : "");
}

void InfoNES_Wait() {
}

void InfoNES_SoundInit(void) {
    RingInit();
}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate) {
    (void)samples_per_sync;
    ESP_LOGI(TAG, "APU %d Hz -> resample 24000 Hz", sample_rate);
    s_resample_phase = 0.0f;
    s_prev_sample = 0;
    return 0;
}

void InfoNES_SoundClose(void) {
}

void InfoNES_SoundOutput(int samples, BYTE* wave1, BYTE* wave2, BYTE* wave3,
                         BYTE* wave4, BYTE* wave5) {
    if (s_audio_ring == nullptr) {
        return;
    }
    const float step = 22050.0f / 24000.0f;
    for (int i = 0; i < samples; i++) {
        int mix = ((int)wave1[i] + wave2[i] + wave3[i] + wave4[i] + wave5[i]) / 5;
        int16_t cur = (int16_t)((mix - 128) * 256);
        while (s_resample_phase < 1.0f) {
            int16_t out_val = (int16_t)(s_prev_sample +
                                        (cur - s_prev_sample) * s_resample_phase);
            s_audio_ring[s_audio_w % kAudioRingLen] = out_val;
            s_audio_w = s_audio_w + 1;
            s_resample_phase += step;
            if (s_audio_w - s_audio_r >= kAudioRingLen) {
                s_audio_r = s_audio_w - (kAudioRingLen - 1024);
                s_audio_overruns = s_audio_overruns + 1;
            }
        }
        s_resample_phase -= 1.0f;
        s_prev_sample = cur;
    }
}

void InfoNES_MessageBox(const char* pszMsg, ...) {
    va_list args;
    va_start(args, pszMsg);
    char buf[256];
    vsnprintf(buf, sizeof(buf), pszMsg, args);
    va_end(args);
    ESP_LOGW(TAG, "%s", buf);
}
