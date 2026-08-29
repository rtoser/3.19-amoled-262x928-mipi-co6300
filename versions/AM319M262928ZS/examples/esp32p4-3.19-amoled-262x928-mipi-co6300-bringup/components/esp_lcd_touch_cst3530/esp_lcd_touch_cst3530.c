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
/* Hynitron cst66xx-series "normal mode" entry, written after every reset (reference driver cst66xx_set_workmode):
 * 0xD0000400 twice = disable the controller's low-power I2C pull-up, then normal-mode selects. */
#define MODE_DISABLE_LP_PULLUP_REG (0xD0000400)
#define MODE_NORMAL_REG_1 (0xD0000000)
#define MODE_NORMAL_REG_2 (0xD0000C00)
#define MODE_NORMAL_REG_3 (0xD0000100)
#define REPORT_HEADER_BYTES (4)     /* [0..1] checksum, [2] report type, [3] key count << 4 | finger count */
#define REPORT_POINT_BYTES (5)      /* x_lo, y_lo, pressure, x_hi << 4 | y_hi, event << 4 | id */
#define REPORT_MAX_ENTRIES (10)     /* CST3530: up to 10 fingers (datasheet), keys share the same slots */
#define REPORT_TYPE_COORDS (0xFF)
#define REPORT_READ_RETRIES (2)
#define REG_ADDR_BYTES (4)
/* Datasheet: bus free time between STOP and the next START (Tbuf) >= 1.5 us */
#define REG_ACCESS_GAP_US (100)

static const char *TAG = "CST3530";

/* After this many consecutive failed reads the bus (if registered) and the controller are reset.
 * Dragging on the AM319M262928ZS produces bursts of NACKs on the shared 3.3 V-pulled-up bus; a
 * failed read must never propagate (esp_lvgl_port aborts on any error from read_data) and a stuck
 * bus must never block the LVGL task (hence transaction_timeout_ms in the IO config). */
#define CST3530_RECOVER_AFTER_ERRORS (3)

/* Single-instance bookkeeping: esp_lcd_touch_t has no private field and config.user_data is used by esp_lvgl_port. */
static struct {
    esp_lcd_touch_handle_t tp;
    i2c_master_bus_handle_t bus;
    esp_lcd_touch_cst3530_stats_t stats;
} s_ctx;

static esp_err_t read_data(esp_lcd_touch_handle_t tp);
static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t del(esp_lcd_touch_handle_t tp);

static esp_err_t write_reg(esp_lcd_touch_handle_t tp, uint32_t reg);
static esp_err_t read_reg(esp_lcd_touch_handle_t tp, uint32_t reg, uint8_t *buf, size_t len);

static esp_err_t reset(esp_lcd_touch_handle_t tp);
static void enter_normal_mode(esp_lcd_touch_handle_t tp);
static void recover(esp_lcd_touch_handle_t tp);

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
    enter_normal_mode(cst3530);
    s_ctx.tp = cst3530;
    memset(&s_ctx.stats, 0, sizeof(s_ctx.stats));
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
    /* Report layout (Hynitron cst66xx protocol, see the viewesmart/esp_lcd_touch_cst3530 reference driver):
     *   [0..1] checksum = 0x55 + sum of all entry bytes (little-endian 16-bit)
     *   [2]    report type, 0xFF = coordinates
     *   [3]    key count (high nibble) | finger count (low nibble)
     *   then REPORT_POINT_BYTES per key/finger: x_lo, y_lo, pressure, x_hi<<4 | y_hi, event<<4 | id (event 0 = lifted)
     * Read the header + first entry (9 bytes), then only the remaining entries: this keeps the bus busy for
     * ~1/7 of a blind 64-byte read, which matters on the shared, 3.3 V-pulled-up touch bus of this module. */
    uint8_t buf[REPORT_HEADER_BYTES + REPORT_POINT_BYTES * REPORT_MAX_ENTRIES] = {0};
    uint8_t finger_num = 0, key_num = 0, report_typ = 0, entries = 0;
    esp_err_t ret = ESP_FAIL;

    for (int attempt = 0; attempt < REPORT_READ_RETRIES; attempt++)
    {
        ret = read_reg(tp, DATA_START_REG, buf, REPORT_HEADER_BYTES + REPORT_POINT_BYTES);
        if (ret != ESP_OK)
        {
            continue;
        }
        report_typ = buf[2];
        finger_num = buf[3] & 0x0F;
        key_num = (buf[3] & 0xF0) >> 4;
        entries = finger_num + key_num;
        if (entries > REPORT_MAX_ENTRIES)
        {
            ret = ESP_ERR_INVALID_RESPONSE;
            continue;
        }
        if (entries > 1)
        {
            ret = esp_lcd_panel_io_rx_param(tp->io, -1, &buf[REPORT_HEADER_BYTES + REPORT_POINT_BYTES],
                                            (entries - 1) * REPORT_POINT_BYTES);
            if (ret != ESP_OK)
            {
                continue;
            }
        }
        uint16_t sum = 0x55;
        for (size_t i = 0; i < (size_t)entries * REPORT_POINT_BYTES; i++)
        {
            sum += buf[REPORT_HEADER_BYTES + i];
        }
        if (sum != (uint16_t)(buf[0] | (buf[1] << 8)))
        {
            ret = ESP_ERR_INVALID_CRC;
            continue;
        }
        break;
    }
    /* Acknowledge the report even after a failed read, otherwise the controller keeps INT asserted and never
     * raises a new edge (event mode would go silent). */
    write_reg(tp, DATA_CLEAR_REG);

    if (ret != ESP_OK)
    {
        portENTER_CRITICAL(&tp->data.lock);
        tp->data.points = 0;
        portEXIT_CRITICAL(&tp->data.lock);
        s_ctx.stats.i2c_errors++;
        s_ctx.stats.consecutive_errors++;
        if (s_ctx.stats.consecutive_errors == 1 || (s_ctx.stats.consecutive_errors % 50) == 0)
        {
            ESP_LOGW(TAG, "report read failed (%s): %lu consecutive, %lu total", esp_err_to_name(ret),
                     (unsigned long)s_ctx.stats.consecutive_errors, (unsigned long)s_ctx.stats.i2c_errors);
        }
        if (s_ctx.stats.consecutive_errors >= CST3530_RECOVER_AFTER_ERRORS)
        {
            recover(tp);
        }
        return ESP_OK; /* report "no touch" instead of an error: callers (esp_lvgl_port) abort on errors */
    }
    s_ctx.stats.consecutive_errors = 0;

    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    if (report_typ == REPORT_TYPE_COORDS && finger_num > 0)
    {
        uint8_t valid_points = 0;
        for (int i = 0; i < finger_num && valid_points < POINT_NUM_MAX; i++)
        {
            const uint8_t *e = &buf[REPORT_HEADER_BYTES + (key_num + i) * REPORT_POINT_BYTES];
            uint8_t event = e[4] >> 4; /* 0 = finger lifted: not a touch point */
            uint16_t x = e[0] | ((uint16_t)(e[3] & 0x0F) << 8);
            uint16_t y = e[1] | ((uint16_t)(e[3] & 0xF0) << 4);
            uint16_t strength = e[2];
            if (event != 0 && x < tp->config.x_max && y < tp->config.y_max && strength > 0)
            {
                tp->data.coords[valid_points].x = x;
                tp->data.coords[valid_points].y = y;
                tp->data.coords[valid_points].strength = strength;
                valid_points++;
            }
        }
        tp->data.points = valid_points;
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
        vTaskDelay(pdMS_TO_TICKS(10));  /* reference driver: 8 ms */
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200)); /* datasheet TPON: reports possible within 200 ms of reset */
    }

    return ESP_OK;
}

/* Register addresses are 32-bit with the top bit set, which cannot be carried in the `int lcd_cmd`
 * argument of the panel IO layer on every IDF release. They are therefore sent as plain parameter
 * bytes (`lcd_cmd = -1`), which both the legacy and the new I2C panel IO implementations honour. */
/* Transfer helpers stay silent on failure; read_data() logs (rate-limited) and recovers. */
static esp_err_t write_reg(esp_lcd_touch_handle_t tp, uint32_t reg)
{
    const uint8_t addr[REG_ADDR_BYTES] = {(reg >> 24) & 0xFF, (reg >> 16) & 0xFF, (reg >> 8) & 0xFF, reg & 0xFF};

    esp_err_t ret = esp_lcd_panel_io_tx_param(tp->io, -1, addr, sizeof(addr));
    esp_rom_delay_us(REG_ACCESS_GAP_US);

    return ret;
}

static esp_err_t read_reg(esp_lcd_touch_handle_t tp, uint32_t reg, uint8_t *buf, size_t len)
{
    esp_err_t ret = write_reg(tp, reg);
    if (ret != ESP_OK)
    {
        return ret;
    }

    return esp_lcd_panel_io_rx_param(tp->io, -1, buf, len);
}

static void recover(esp_lcd_touch_handle_t tp)
{
    s_ctx.stats.recoveries++;
    ESP_LOGW(TAG, "recovery #%lu: %s + controller reset", (unsigned long)s_ctx.stats.recoveries,
             s_ctx.bus ? "I2C bus reset" : "(no bus registered)");
    if (s_ctx.bus)
    {
        i2c_master_bus_reset(s_ctx.bus); /* 9 SCL pulses + STOP frees a slave holding SDA low */
    }
    reset(tp);                           /* RST pulse when wired; the adapter RC keeps it released otherwise */
    enter_normal_mode(tp);
    s_ctx.stats.consecutive_errors = 0;
}

static void enter_normal_mode(esp_lcd_touch_handle_t tp)
{
    esp_err_t ret = write_reg(tp, MODE_DISABLE_LP_PULLUP_REG);
    vTaskDelay(pdMS_TO_TICKS(1));
    ret |= write_reg(tp, MODE_DISABLE_LP_PULLUP_REG);
    ret |= write_reg(tp, MODE_NORMAL_REG_1);
    ret |= write_reg(tp, MODE_NORMAL_REG_2);
    ret |= write_reg(tp, MODE_NORMAL_REG_3);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "entering normal mode failed (%s); the controller still reports touches in its default mode",
                 esp_err_to_name(ret));
    }
}

esp_err_t esp_lcd_touch_cst3530_set_i2c_bus(esp_lcd_touch_handle_t tp, i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(tp && tp == s_ctx.tp, ESP_ERR_INVALID_ARG, TAG, "unknown touch handle");
    s_ctx.bus = bus;
    return ESP_OK;
}

void esp_lcd_touch_cst3530_get_stats(esp_lcd_touch_handle_t tp, esp_lcd_touch_cst3530_stats_t *out)
{
    if (out)
    {
        *out = (tp == s_ctx.tp) ? s_ctx.stats : (esp_lcd_touch_cst3530_stats_t){0};
    }
}
