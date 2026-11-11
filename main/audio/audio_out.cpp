#include "audio_out.h"

#include <cmath>
#include <cstring>

#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>

#include "board_pins.h"

static const char* TAG = "AudioOut";

namespace audio {
namespace {

constexpr int kSampleRate = 24000;
constexpr int kDmaDescNum = 6;
constexpr int kDmaFrameNum = 240;  // 10 ms at 24 kHz
constexpr uint8_t kEs8388Addr = 0x20;  // 8-bit form; esp_codec_dev shifts to 0x10

i2s_chan_handle_t s_tx = nullptr;
i2s_chan_handle_t s_rx = nullptr;
const audio_codec_data_if_t* s_data_if = nullptr;
const audio_codec_ctrl_if_t* s_ctrl_if = nullptr;
const audio_codec_if_t* s_codec_if = nullptr;
const audio_codec_gpio_if_t* s_gpio_if = nullptr;
esp_codec_dev_handle_t s_out = nullptr;
bool s_opened = false;
int s_volume = 70;

/* es8388_codec_new() does not register set_reg, so write the four analog
 * output volume registers through the I2C control interface. The board's
 * speaker is on OUT2 (regs 48/49), which otherwise stays at -45 dB. */
void ApplyAnalogVolume() {
    if (s_ctrl_if == nullptr || s_ctrl_if->write_reg == nullptr) {
        return;
    }
    uint8_t zero_db = 0x1E;
    for (int reg = 46; reg <= 49; reg++) {
        s_ctrl_if->write_reg(s_ctrl_if, reg, 1, &zero_db, 1);
    }
    if (s_ctrl_if->read_reg != nullptr) {
        int readback = 0;
        s_ctrl_if->read_reg(s_ctrl_if, 48, 1, &readback, 1);
        ESP_LOGI(TAG, "analog volume 0x2e..0x31 set to 0x1E (0x30 readback=0x%02x)", readback);
    }
}

bool CreateDuplexChannels() {
    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = I2S_NUM_0;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = kDmaDescNum;
    chan_cfg.dma_frame_num = kDmaFrameNum;
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = false;
    chan_cfg.intr_priority = 0;
    if (i2s_new_channel(&chan_cfg, &s_tx, &s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = kSampleRate;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
    std_cfg.slot_cfg.left_align = true;
    std_cfg.slot_cfg.big_endian = false;
    std_cfg.slot_cfg.bit_order_lsb = false;
    std_cfg.gpio_cfg.mclk = board::kI2sMclk;
    std_cfg.gpio_cfg.bclk = board::kI2sBclk;
    std_cfg.gpio_cfg.ws = board::kI2sWs;
    std_cfg.gpio_cfg.dout = board::kI2sDout;
    std_cfg.gpio_cfg.din = board::kI2sDin;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_init_std_mode(s_rx, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
        return false;
    }
    if (i2s_channel_enable(s_tx) != ESP_OK || i2s_channel_enable(s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed");
        return false;
    }
    return true;
}

}  // namespace

bool Init(i2c_master_bus_handle_t i2c_bus) {
    if (s_out != nullptr) {
        return true;
    }
    if (!CreateDuplexChannels()) {
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = s_rx;
    i2s_cfg.tx_handle = s_tx;
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == nullptr) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return false;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = (uint8_t)I2C_NUM_0;
    i2c_cfg.addr = kEs8388Addr;
    i2c_cfg.bus_handle = i2c_bus;
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_ctrl_if == nullptr) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return false;
    }

    s_gpio_if = audio_codec_new_gpio();

    es8388_codec_cfg_t cfg = {};
    cfg.ctrl_if = s_ctrl_if;
    cfg.gpio_if = s_gpio_if;
    /* Keep the exact configuration proven on this board (microphone unused;
     * ES8388 master_mode=true works with the vendor hardware). */
    cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    cfg.master_mode = true;
    cfg.pa_pin = GPIO_NUM_NC;
    cfg.pa_reverted = false;
    cfg.hw_gain.pa_voltage = 5.0;
    cfg.hw_gain.codec_dac_voltage = 3.3;
    s_codec_if = es8388_codec_new(&cfg);
    if (s_codec_if == nullptr) {
        ESP_LOGE(TAG, "es8388_codec_new failed");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = s_codec_if;
    dev_cfg.data_if = s_data_if;
    s_out = esp_codec_dev_new(&dev_cfg);
    if (s_out == nullptr) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return false;
    }
    esp_codec_set_disable_when_closed(s_out, false);
    ESP_LOGI(TAG, "ES8388 ready (24 kHz mono out)");
    return true;
}

bool Start() {
    if (s_out == nullptr) {
        return false;
    }
    if (s_opened) {
        /* Do not reopen the shared duplex interface on IDF 6: it can leave
         * playback silent. Just unmute and re-apply the analog level. */
        esp_codec_dev_set_out_vol(s_out, s_volume);
        esp_codec_dev_set_out_mute(s_out, false);
        ApplyAnalogVolume();
        ESP_LOGI(TAG, "output resumed, volume %d", s_volume);
        return true;
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.channel_mask = 0;
    fs.sample_rate = kSampleRate;
    fs.mclk_multiple = 0;  // 256 * fs
    int err = esp_codec_dev_open(s_out, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", err);
        return false;
    }
    esp_codec_dev_set_out_vol(s_out, s_volume);
    esp_codec_dev_set_out_mute(s_out, false);
    /* The driver leaves the analog output at -45 dB; write all four analog
     * output controls (HP L/R and speaker L/R) to 0 dB. */
    ApplyAnalogVolume();
    s_opened = true;
    ESP_LOGI(TAG, "output started, volume %d", s_volume);
    return true;
}

void Stop() {
    if (s_out == nullptr || !s_opened) {
        return;
    }
    /* Mute only. Closing the shared TX/RX interface and reopening it after
     * Wi-Fi provisioning leaves playback silent on ESP-IDF 6. */
    esp_codec_dev_set_out_mute(s_out, true);
}

void SetVolume(int volume_0_100) {
    if (volume_0_100 < 0) {
        volume_0_100 = 0;
    }
    if (volume_0_100 > 100) {
        volume_0_100 = 100;
    }
    s_volume = volume_0_100;
    if (s_out != nullptr && s_opened) {
        esp_codec_dev_set_out_vol(s_out, s_volume);
    }
}

size_t Write(const int16_t* data, size_t samples) {
    if (s_out == nullptr || !s_opened || data == nullptr || samples == 0) {
        return 0;
    }
    int ret = esp_codec_dev_write(s_out, (void*)data, (int)(samples * sizeof(int16_t)));
    if (ret != ESP_CODEC_DEV_OK) {
        return 0;
    }
    return samples;
}

void PlayTestTone(int ms) {
    if (ms <= 0 || !Start()) {
        return;
    }
    constexpr int kRate = 24000;
    constexpr int kChunk = 240;  // 10 ms
    const int total = kRate * ms / 1000;
    int16_t buf[kChunk];
    for (int done = 0; done < total;) {
        int n = total - done;
        if (n > kChunk) {
            n = kChunk;
        }
        for (int i = 0; i < n; i++) {
            const float phase = 2.0f * (float)M_PI * 1000.0f * (done + i) / kRate;
            buf[i] = (int16_t)(sinf(phase) * 8000.0f);
        }
        if (Write(buf, n) == 0) {
            break;
        }
        done += n;
    }
    Stop();
    ESP_LOGI(TAG, "test tone done (%d ms)", ms);
}

}  // namespace audio
