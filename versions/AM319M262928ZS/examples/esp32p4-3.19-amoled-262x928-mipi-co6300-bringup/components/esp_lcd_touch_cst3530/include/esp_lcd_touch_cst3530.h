/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2025 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ESP LCD touch: cst3530
 */

#pragma once

#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Create a new CST3530 touch driver
     *
     * @param io      LCD panel IO handle created with `esp_lcd_new_panel_io_i2c()` on an `i2c_master_bus_handle_t`
     *                (new `driver/i2c_master.h` driver), using `ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG()`
     * @param config  Touch configuration
     * @param out_touch Returned touch handle
     */
    esp_err_t esp_lcd_touch_new_i2c_cst3530(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *out_touch);

#define ESP_LCD_TOUCH_IO_I2C_CST3530_ADDRESS (0x58)

/* CST3530 registers are addressed with 4 bytes; the driver sends them as raw bytes, so the
 * command width here only matters if a caller issues its own `esp_lcd_panel_io_*` transfers. */
#define ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG()             \
    {                                                     \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST3530_ADDRESS, \
        .scl_speed_hz = 400 * 1000,                       \
        .control_phase_bytes = 1,                         \
        .dc_bit_offset = 0,                               \
        .lcd_cmd_bits = 32,                               \
        .flags =                                          \
            {                                             \
                .disable_control_phase = 1,               \
            },                                            \
    }

#ifdef __cplusplus
}
#endif
