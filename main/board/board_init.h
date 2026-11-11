#pragma once

#include <cstdint>

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

namespace board {

struct Handles {
    i2c_master_bus_handle_t i2c = nullptr;
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
};

/* I2C + XL9555 + SPI + ST7789 panel init. Call once from app_main. */
esp_err_t Init();

Handles& Get();

void SetBacklight(bool on);

/* XL9555 helpers (bit 0..15, P0_x = 0..7, P1_x = 8..15) */
esp_err_t Xl9555SetBit(int bit, bool level);

/* BOOT button, active low */
bool BootButtonDown();

}  // namespace board
