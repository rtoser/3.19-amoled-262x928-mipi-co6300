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
#include "driver/i2c_master.h"

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

    /**
     * @brief Register the I2C bus so the driver can recover a stuck bus (`i2c_master_bus_reset()`)
     *        after repeated transfer failures. Optional; without it recovery only resets the controller via RST.
     */
    esp_err_t esp_lcd_touch_cst3530_set_i2c_bus(esp_lcd_touch_handle_t tp, i2c_master_bus_handle_t bus);

    typedef struct {
        uint32_t i2c_errors;         /*!< Failed reads since boot */
        uint32_t consecutive_errors; /*!< Failed reads since the last good one */
        uint32_t recoveries;         /*!< Bus reset + controller reset cycles performed */
    } esp_lcd_touch_cst3530_stats_t;

    /** @brief I2C failure / recovery counters (a read failure never propagates as an error, see the .c file) */
    void esp_lcd_touch_cst3530_get_stats(esp_lcd_touch_handle_t tp, esp_lcd_touch_cst3530_stats_t *out);

#define ESP_LCD_TOUCH_IO_I2C_CST3530_ADDRESS (0x58)

/* CST3530 registers are addressed with 4 bytes; the driver sends them as raw bytes, so the
 * command width here only matters if a caller issues its own `esp_lcd_panel_io_*` transfers. */
#define ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG()             \
    {                                                     \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST3530_ADDRESS, \
        .scl_speed_hz = 400 * 1000,                       \
        .transaction_timeout_ms = 20,                     \
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
