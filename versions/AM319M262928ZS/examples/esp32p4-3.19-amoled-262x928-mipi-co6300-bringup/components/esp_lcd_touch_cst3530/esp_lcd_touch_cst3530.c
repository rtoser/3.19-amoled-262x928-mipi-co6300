/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"

#include "esp_lcd_touch_cst3530.h"

#ifdef CONFIG_ESP_LCD_TOUCH_MAX_POINTS
#define POINT_NUM_MAX CONFIG_ESP_LCD_TOUCH_MAX_POINTS
#else
#define POINT_NUM_MAX (5)
#endif

#define DATA_START_REG (0xD0070000)
#define DATA_CLEAR_REG (0xD00002AB)
#define REG_ADDR_BYTES (4)
/* Datasheet: bus free time between STOP and the next START (Tbuf) >= 1.5 us */
#define REG_ACCESS_GAP_US (100)

static const char *TAG = "CST3530";

static esp_err_t read_data(esp_lcd_touch_handle_t tp);
static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t del(esp_lcd_touch_handle_t tp);

static esp_err_t write_reg(esp_lcd_touch_handle_t tp, uint32_t reg);
static esp_err_t read_reg(esp_lcd_touch_handle_t tp, uint32_t reg, uint8_t *buf, size_t len);

static esp_err_t reset(esp_lcd_touch_handle_t tp);

esp_err_t esp_lcd_touch_new_i2c_cst3530(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp)
{
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_ARG, TAG, "Invalid io");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_INVALID_ARG, TAG, "Invalid touch handle");

    /* Prepare main structure */
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t cst3530 = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_GOTO_ON_FALSE(cst3530, ESP_ERR_NO_MEM, err, TAG, "Touch handle malloc failed");

    /* Communication interface */
    cst3530->io = io;
    /* Only supported callbacks are set */
    cst3530->read_data = read_data;
    cst3530->get_xy = get_xy;
    cst3530->del = del;
    /* Mutex */
    cst3530->data.lock.owner = portMUX_FREE_VAL;
    /* Save config */
    memcpy(&cst3530->config, config, sizeof(esp_lcd_touch_config_t));

    /* Prepare pin for touch interrupt */
    if (cst3530->config.int_gpio_num != GPIO_NUM_NC)
    {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = (cst3530->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
            .pin_bit_mask = BIT64(cst3530->config.int_gpio_num)};
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO intr config failed");

        /* Register interrupt callback */
        if (cst3530->config.interrupt_callback)
        {
            esp_lcd_touch_register_interrupt_callback(cst3530, cst3530->config.interrupt_callback);
        }
    }
    /* Prepare pin for touch controller reset */
    if (cst3530->config.rst_gpio_num != GPIO_NUM_NC)
    {
        const gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(cst3530->config.rst_gpio_num)};
        ESP_GOTO_ON_ERROR(gpio_config(&rst_gpio_config), err, TAG, "GPIO reset config failed");
    }
    /* Reset controller */
    ESP_GOTO_ON_ERROR(reset(cst3530), err, TAG, "Reset failed");
    *tp = cst3530;

    return ESP_OK;
err:
    if (cst3530)
    {
        del(cst3530);
    }
    ESP_LOGE(TAG, "Initialization failed!");
    return ret;
}

static esp_err_t read_data(esp_lcd_touch_handle_t tp)
{
    uint8_t buf[64] = {0};
    uint8_t finger_num = 0, key_num = 0, report_typ = 0;

    esp_err_t ret = read_reg(tp, DATA_START_REG, buf, sizeof(buf));
    if (ret != ESP_OK)
    {
        portENTER_CRITICAL(&tp->data.lock);
        tp->data.points = 0;
        portEXIT_CRITICAL(&tp->data.lock);
        return ret;
    }

    report_typ = buf[2];
    finger_num = buf[3] & 0x0F;
    key_num = ((buf[3] & 0xF0) >> 4); // 用不到

    write_reg(tp, DATA_CLEAR_REG);
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = (finger_num > POINT_NUM_MAX ? POINT_NUM_MAX : finger_num);

    if (report_typ == 0xff && finger_num > 0)
    {
        uint8_t valid_points = 0;
        for (int i = 0; i < tp->data.points; i++)
        {
            int index = (key_num + i) * 5;
            uint16_t x = buf[index + 4] + ((uint16_t)(buf[index + 7] & 0x0F) << 8);
            uint16_t y = buf[index + 5] + ((uint16_t)(buf[index + 7] & 0xF0) << 4);
            uint16_t strength = buf[index + 6];

            // 基本有效性过滤：坐标在范围内，压力非零
            if (x < tp->config.x_max && y < tp->config.y_max && strength > 0)
            {
                tp->data.coords[valid_points].x = x;
                tp->data.coords[valid_points].y = y;
                tp->data.coords[valid_points].strength = strength;
                valid_points++;
            }
        }
        tp->data.points = valid_points;
    }
    else
    {
        tp->data.points = 0;
    }

    portEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
}

static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);

    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);

    for (size_t i = 0; i < *point_num; i++)
    {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength)
        {
            strength[i] = tp->data.coords[i].strength;
        }
    }

    tp->data.points = 0; // 清空缓存
    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t del(esp_lcd_touch_handle_t tp)
{
    /* Reset GPIO pin settings */
    if (tp->config.int_gpio_num != GPIO_NUM_NC)
    {
        gpio_reset_pin(tp->config.int_gpio_num);
        if (tp->config.interrupt_callback)
        {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC)
    {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    /* Release memory */
    free(tp);

    return ESP_OK;
}

static esp_err_t reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC)
    {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    return ESP_OK;
}

/* Register addresses are 32-bit with the top bit set, which cannot be carried in the `int lcd_cmd`
 * argument of the panel IO layer on every IDF release. They are therefore sent as plain parameter
 * bytes (`lcd_cmd = -1`), which both the legacy and the new I2C panel IO implementations honour. */
static esp_err_t write_reg(esp_lcd_touch_handle_t tp, uint32_t reg)
{
    const uint8_t addr[REG_ADDR_BYTES] = {(reg >> 24) & 0xFF, (reg >> 16) & 0xFF, (reg >> 8) & 0xFF, reg & 0xFF};

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(tp->io, -1, addr, sizeof(addr)), TAG, "I2C write failed");
    esp_rom_delay_us(REG_ACCESS_GAP_US);

    return ESP_OK;
}

static esp_err_t read_reg(esp_lcd_touch_handle_t tp, uint32_t reg, uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_ERROR(write_reg(tp, reg), TAG, "Set register address failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(tp->io, -1, buf, len), TAG, "I2C read failed");

    return ESP_OK;
}
