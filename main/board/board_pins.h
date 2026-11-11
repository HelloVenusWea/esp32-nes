#pragma once

#include <driver/gpio.h>

namespace board {

/* 正点原子 ATK-DNESP32S3 板级引脚（与桌面搭子工程实测一致） */

// I2C: ES8388 + XL9555
constexpr gpio_num_t kI2cSda = GPIO_NUM_41;
constexpr gpio_num_t kI2cScl = GPIO_NUM_42;

// ST7789 2.4" 320x240, SPI2
constexpr gpio_num_t kLcdSclk = GPIO_NUM_12;
constexpr gpio_num_t kLcdMosi = GPIO_NUM_11;
constexpr gpio_num_t kLcdDc = GPIO_NUM_40;
constexpr gpio_num_t kLcdCs = GPIO_NUM_21;

// ES8388 I2S
constexpr gpio_num_t kI2sMclk = GPIO_NUM_3;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_46;
constexpr gpio_num_t kI2sWs = GPIO_NUM_9;
constexpr gpio_num_t kI2sDout = GPIO_NUM_10;
constexpr gpio_num_t kI2sDin = GPIO_NUM_14;

// XL9555 IO expander
constexpr uint8_t kXl9555Addr = 0x20;  // 7-bit
constexpr int kBacklightBit = 8;       // P1_0
constexpr int kLcdResetBit = 2;        // P0_2

// BOOT button / LED
constexpr gpio_num_t kBootButton = GPIO_NUM_0;
constexpr gpio_num_t kLed = GPIO_NUM_1;

// Panel geometry (after swap_xy)
constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;

// NES frame geometry
constexpr int kNesWidth = 256;
constexpr int kNesHeight = 240;

}  // namespace board
