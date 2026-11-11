#include "board_init.h"

#include <cstring>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>
#include <esp_log.h>

#include "board_pins.h"

static const char* TAG = "Board";

namespace board {
namespace {

Handles s_handles;
i2c_master_dev_handle_t s_xl9555 = nullptr;

esp_err_t XlWrite(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_xl9555, buf, sizeof(buf), 100);
}

esp_err_t XlRead(uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(s_xl9555, &reg, 1, value, 1, 100);
}

}  // namespace

esp_err_t Xl9555SetBit(int bit, bool level) {
    if (s_xl9555 == nullptr || bit < 0 || bit > 15) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t reg = bit < 8 ? 0x02 : 0x03;
    const int index = bit < 8 ? bit : bit - 8;
    uint8_t data = 0;
    ESP_RETURN_ON_ERROR(XlRead(reg, &data), TAG, "XL9555 read 0x%02x failed", reg);
    if (level) {
        data |= (uint8_t)(1u << index);
    } else {
        data &= (uint8_t)~(1u << index);
    }
    return XlWrite(reg, data);
}

void SetBacklight(bool on) {
    esp_err_t err = Xl9555SetBit(kBacklightBit, on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight %s failed: %s", on ? "on" : "off", esp_err_to_name(err));
    }
}

esp_err_t Init() {
    /* ---------- I2C ---------- */
    i2c_master_bus_config_t i2c_cfg = {};
    i2c_cfg.i2c_port = I2C_NUM_0;
    i2c_cfg.sda_io_num = kI2cSda;
    i2c_cfg.scl_io_num = kI2cScl;
    i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt = 7;
    i2c_cfg.intr_priority = 0;
    i2c_cfg.trans_queue_depth = 0;
    i2c_cfg.flags.enable_internal_pullup = 1;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_handles.i2c), TAG, "i2c bus init failed");

    i2c_device_config_t xl_cfg = {};
    xl_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    xl_cfg.device_address = kXl9555Addr;
    xl_cfg.scl_speed_hz = 400 * 1000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_handles.i2c, &xl_cfg, &s_xl9555), TAG,
                        "XL9555 add failed");

    /* P0_0/P0_1 output, P1_4..P1_7 output (same as the vendor demo) */
    ESP_RETURN_ON_ERROR(XlWrite(0x06, 0x03), TAG, "XL9555 config port0 failed");
    ESP_RETURN_ON_ERROR(XlWrite(0x07, 0xF0), TAG, "XL9555 config port1 failed");
    /* Backlight off until the panel is initialized */
    Xl9555SetBit(kBacklightBit, false);

    /* ---------- BOOT button ---------- */
    gpio_config_t boot_cfg = {};
    boot_cfg.pin_bit_mask = 1ULL << kBootButton;
    boot_cfg.mode = GPIO_MODE_INPUT;
    boot_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    boot_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    boot_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&boot_cfg), TAG, "boot button config failed");

    /* ---------- SPI + ST7789 ---------- */
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = kLcdMosi;
    bus_cfg.miso_io_num = GPIO_NUM_NC;
    bus_cfg.sclk_io_num = kLcdSclk;
    bus_cfg.quadwp_io_num = GPIO_NUM_NC;
    bus_cfg.quadhd_io_num = GPIO_NUM_NC;
    bus_cfg.max_transfer_sz = kScreenWidth * kScreenHeight * 2 + 64;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG,
                        "spi bus init failed");

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = kLcdCs;
    io_cfg.dc_gpio_num = kLcdDc;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = 80 * 1000 * 1000;
    /* Depth 1: draw_bitmap waits for the previous transfer, so the frame buffer
     * handed back to the emulator is never still being DMA'd. */
    io_cfg.trans_queue_depth = 1;
    io_cfg.flags.psram_dma_direct = 1;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_handles.panel_io), TAG,
                        "panel io init failed");

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = GPIO_NUM_NC;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_handles.panel_io, &panel_cfg, &s_handles.panel),
                        TAG, "panel init failed");

    /* Panel reset/power is wired through XL9555: P1_0 = power enable,
     * P0_2 = reset release (vendor demo sequence). */
    esp_lcd_panel_reset(s_handles.panel);
    ESP_RETURN_ON_ERROR(Xl9555SetBit(kBacklightBit, true), TAG, "panel power enable failed");
    ESP_RETURN_ON_ERROR(Xl9555SetBit(kLcdResetBit, false), TAG, "panel reset release failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_handles.panel), TAG, "panel init failed");
    esp_lcd_panel_invert_color(s_handles.panel, true);
    esp_lcd_panel_swap_xy(s_handles.panel, true);
    esp_lcd_panel_mirror(s_handles.panel, true, false);
    esp_lcd_panel_disp_on_off(s_handles.panel, true);

    SetBacklight(true);
    ESP_LOGI(TAG, "ST7789 %dx%d @80MHz ready", kScreenWidth, kScreenHeight);
    return ESP_OK;
}

Handles& Get() {
    return s_handles;
}

bool BootButtonDown() {
    return gpio_get_level(kBootButton) == 0;
}

}  // namespace board
