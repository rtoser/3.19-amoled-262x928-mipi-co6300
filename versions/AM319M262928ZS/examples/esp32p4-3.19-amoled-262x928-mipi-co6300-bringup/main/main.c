/*
 * SPDX-FileCopyrightText: Copyright 2026 OSPTEK
 * SPDX-License-Identifier: CC-BY-4.0
 *
 * https://github.com/osptek
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co6300.h"
#include "esp_lcd_touch_cst3530.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "esp_lv_adapter_input.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

#include "flip_clock.h"
#include "perf_probe.h"

static const char *TAG = "Main";

/* 显示栈：esp_lvgl_adapter TRIPLE_PARTIAL（三缓冲局部渲染防撕裂）+ PPA 90° 旋转，
 * 逻辑横屏 928×262（面板物理扫描仍是竖向 262×928）。选型与实测见 docs/perf 与
 * README「渲染模式」章：横屏 benchmark 55 FPS、0 看门狗，触摸四方向已验证。 */
#define EXAMPLE_ADAPTER_TEAR_MODE ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI /* TRIPLE_PARTIAL */
#define EXAMPLE_ADAPTER_ROTATION  ESP_LV_ADAPTER_ROTATE_90
#define EXAMPLE_ROTATION_DEG      90

/* DPI 时钟源为 PLL_F240M，整数分频：240/12 = 20 MHz。刷新率 = 20e6 / (330 × 1010) = 60.01 Hz，
 * 落在规格书 F_frm 58.2–61.8 Hz 窗口内（16 MHz 时只有 51.1 Hz，低于规格下限）。 */
#define MIPI_DSI_DPI_CLK_MHZ 20

#define MIPI_DSI_LCD_H_RES 262
#define MIPI_DSI_LCD_V_RES 928

#define MIPI_DSI_LCD_HSYNC 4
#define MIPI_DSI_LCD_HBP 32
#define MIPI_DSI_LCD_HFP 32
#define MIPI_DSI_LCD_VSYNC 4
#define MIPI_DSI_LCD_VBP 8
#define MIPI_DSI_LCD_VFP 70  /* 拉长前沿消隐把帧率凑到 60 Hz；VSYNC/VBP 与 GOA 时序绑定，不动 */

#define TEST_MIPI_DSI_PHY_PWR_LDO_CHAN 3
#define TEST_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500

/* 引脚在 menuconfig -> "Example Configuration (AM319M262928ZS bring-up)" 中配置，默认值对应
 * 转接板实拍所用的开发板；其他底板（如 ESP32-P4 模组基础底板 V1.3）的取值见 README「硬件连接」。
 * LCD_RST = -1 表示不接线、改用 DCS 软件复位；VCI_EN / TP_RST / TP_INT = -1 表示该线未连接。 */
#define EXAMPLE_PIN_NUM_LCD_RST    CONFIG_EXAMPLE_PIN_NUM_LCD_RST
#define EXAMPLE_PIN_NUM_LCD_VCI_EN CONFIG_EXAMPLE_PIN_NUM_LCD_VCI_EN
#define EXAMPLE_PIN_NUM_TOUCH_SCL  CONFIG_EXAMPLE_PIN_NUM_TOUCH_SCL
#define EXAMPLE_PIN_NUM_TOUCH_SDA  CONFIG_EXAMPLE_PIN_NUM_TOUCH_SDA
#define EXAMPLE_PIN_NUM_TOUCH_RST  CONFIG_EXAMPLE_PIN_NUM_TOUCH_RST
#define EXAMPLE_PIN_NUM_TOUCH_INT  CONFIG_EXAMPLE_PIN_NUM_TOUCH_INT

#define TOUCH_HOST I2C_NUM_0

static esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
static esp_lcd_panel_handle_t mipi_dpi_panel = NULL;
static i2c_master_bus_handle_t touch_i2c_bus = NULL;
static esp_lcd_touch_handle_t tp = NULL;

static lv_display_t *lvgl_disp = NULL;

static const co6300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0xF4, (uint8_t[]){0x5A}, 1, 0},
    {0xF5, (uint8_t[]){0x59}, 1, 0},
    {0xFE, (uint8_t[]){0x80}, 1, 0},
    {0x03, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x77}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x05}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x03, 0x9F}, 4, 0},
    // CTRL Display：BC_EN(D5)=1 亮度控制使能 + DIM_EN(D3)=1 亮度渐变（写 51h 平滑过渡；
    // 芯片复位默认即 28h，厂商脚本的 20h 反而关了渐变）
    {0x53, (uint8_t[]){0x28}, 1, 0},
    // 亮度（WRDISBV）：默认 60%，menuconfig 可调；厂商 demo 的 0xFF 只适合展台
    {0x51, (uint8_t[]){CONFIG_EXAMPLE_PANEL_BRIGHTNESS}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x11, NULL, 0, 60},
    {0x29, NULL, 0, 0},
};

static void lcd_vci_en_init(void) {
    if (EXAMPLE_PIN_NUM_LCD_VCI_EN < 0) {
        ESP_LOGW(TAG, "VCI_EN not driven (pin = -1); relying on the adapter board pull-up");
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_LCD_VCI_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    gpio_set_level(EXAMPLE_PIN_NUM_LCD_VCI_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(60));

    gpio_set_level(EXAMPLE_PIN_NUM_LCD_VCI_EN, 1);

    vTaskDelay(pdMS_TO_TICKS(50));
}

static void i2c_scan(i2c_master_bus_handle_t bus) {
    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "Found I2C device at address 0x%02x", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan completed.");
}

esp_err_t app_lcd_init() {
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;

    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = TEST_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = TEST_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 1,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 360,
    };
    ESP_LOGI(TAG, "esp_lcd_new_dsi_bus!");
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_LOGI(TAG, "esp_lcd_new_panel_io_dbi!");
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = MIPI_DSI_DPI_CLK_MHZ,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = esp_lv_adapter_get_required_frame_buffer_count(
            EXAMPLE_ADAPTER_TEAR_MODE, EXAMPLE_ADAPTER_ROTATION),
        .video_timing =
            {
                .h_size = MIPI_DSI_LCD_H_RES,
                .v_size = MIPI_DSI_LCD_V_RES,
                .hsync_back_porch = MIPI_DSI_LCD_HBP,
                .hsync_pulse_width = MIPI_DSI_LCD_HSYNC,
                .hsync_front_porch = MIPI_DSI_LCD_HFP,
                .vsync_back_porch = MIPI_DSI_LCD_VBP,
                .vsync_pulse_width = MIPI_DSI_LCD_VSYNC,
                .vsync_front_porch = MIPI_DSI_LCD_VFP,
            },
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        .flags =
            {
                .use_dma2d = true,
            }
#endif
    };

    co6300_vendor_config_t vendor_config = {0};
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    vendor_config.flags.use_mipi_interface = 1;
    vendor_config.mipi_config.dsi_bus = mipi_dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_co6300(mipi_dbi_io, &lcd_dev_config, &mipi_dpi_panel));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    // IDF 6.0 removed `flags.use_dma2d`; DMA2D is enabled on the DPI panel explicitly instead
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(mipi_dpi_panel));
#endif
    esp_lcd_panel_reset(mipi_dpi_panel);
    esp_lcd_panel_init(mipi_dpi_panel);

    assert(mipi_dbi_io);
    assert(mipi_dpi_panel);

    return ESP_OK;
}

esp_err_t app_touch_init() {
    ESP_LOGI(TAG, "Initialize I2C bus");

    // 确保触摸 RST 释放，否则 I2C 无应答（未接线时由转接板 RC 上拉释放）
    if (EXAMPLE_PIN_NUM_TOUCH_RST >= 0) {
        const gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_TOUCH_RST,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(EXAMPLE_PIN_NUM_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    const i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = TOUCH_HOST,
        .sda_io_num = EXAMPLE_PIN_NUM_TOUCH_SDA,
        .scl_io_num = EXAMPLE_PIN_NUM_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &touch_i2c_bus));
    i2c_scan(touch_i2c_bus);

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(touch_i2c_bus, &tp_io_config, &tp_io_handle));
    // 给 INT 脚上拉，避免悬空噪声误报
    if (EXAMPLE_PIN_NUM_TOUCH_INT >= 0) {
        gpio_set_pull_mode(EXAMPLE_PIN_NUM_TOUCH_INT, GPIO_PULLUP_ONLY);
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = MIPI_DSI_LCD_H_RES,
        .y_max = MIPI_DSI_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_PIN_NUM_TOUCH_RST,
        .int_gpio_num = EXAMPLE_PIN_NUM_TOUCH_INT,
        .levels =
            {
                .reset = 0,
                .interrupt = 0,
            },
        .flags =
            {
                /* esp_lcd_touch 软件路径先在物理坐标系 mirror、后 swap，因此
                 * 旋转的逆映射为：90°=mirror_x+swap，180°=双 mirror，
                 * 270°=mirror_y+swap（实测于横屏 widgets，四方向+四角通过）。 */
                .swap_xy = (EXAMPLE_ROTATION_DEG == 90 || EXAMPLE_ROTATION_DEG == 270),
                .mirror_x = (EXAMPLE_ROTATION_DEG == 90 || EXAMPLE_ROTATION_DEG == 180),
                .mirror_y = (EXAMPLE_ROTATION_DEG == 270 || EXAMPLE_ROTATION_DEG == 180),
            },
    };
    ESP_LOGI(TAG, "Initialize touch controller");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst3530(tp_io_handle, &tp_cfg, &tp), TAG, "touch init failed");
    // 让驱动在连续 I2C 失败后能复位总线（9 个 SCL 脉冲）再复位触摸芯片
    return esp_lcd_touch_cst3530_set_i2c_bus(tp, touch_i2c_bus);
}

esp_err_t app_lvgl_init() {
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.tick_period_ms = 1;  /* 1 ms 时基；5 ms 会把 16 ms 的刷新周期量化成 20 ms（50 FPS） */
    cfg.task_core_id = 1;    /* UI 钉 core 1，core 0 留给日后的 hosted 网络栈 */
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&cfg), TAG, "adapter init failed");

    ESP_LOGD(TAG, "Add LCD screen");
    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
        mipi_dpi_panel, mipi_dbi_io, MIPI_DSI_LCD_H_RES, MIPI_DSI_LCD_V_RES,
        EXAMPLE_ADAPTER_ROTATION);
    disp_cfg.tear_avoid_mode = EXAMPLE_ADAPTER_TEAR_MODE;
    disp_cfg.profile.buffer_height = 128;  /* 局部渲染条带 262×128×2 B = 67 KB，cache 行整数倍 */
    lvgl_disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_RETURN_ON_FALSE(lvgl_disp, ESP_FAIL, TAG, "register display failed");

    esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(lvgl_disp, tp);
    ESP_RETURN_ON_FALSE(esp_lv_adapter_register_touch(&touch_cfg), ESP_FAIL, TAG,
                        "register touch failed");

    return esp_lv_adapter_start();
}

/* ---- 夜间自动降亮（用户 2026-08-30 批准的 IDLE 策略第一阶段） ----
 * 判定输入只有两个：墙钟时段 + 触摸活动（板上无光感）。白天保持常亮——
 * 时钟变暗是反功能；夜间(22:00–07:00)无触摸 30 s 平滑降到 NIGHT 亮度，
 * 触摸即恢复。执行动作只有重写 51h（53h 已开 DIM_EN，过渡平滑）；
 * 面板 Idle Mode(39h,15Hz) 在 DPI video 模式下的行为待实验，见 README。 */
#define BRIGHTNESS_ACTIVE     CONFIG_EXAMPLE_PANEL_BRIGHTNESS
#define BRIGHTNESS_NIGHT      0x30
#define NIGHT_BEGIN_HOUR      22
#define NIGHT_END_HOUR        7
#define NIGHT_DIM_INACTIVE_MS 30000

static void panel_set_brightness(uint8_t level) {
    static uint8_t current = BRIGHTNESS_ACTIVE;
    if (level == current) {
        return;
    }
    current = level;
    if (esp_lcd_panel_io_tx_param(mipi_dbi_io, 0x51, &level, 1) != ESP_OK) {
        ESP_LOGW(TAG, "brightness write failed");
    }
}

static void brightness_policy_cb(lv_timer_t *timer) {
    (void)timer;
    time_t now_ts = time(NULL);
    struct tm now;
    localtime_r(&now_ts, &now);
    const bool night = now.tm_hour >= NIGHT_BEGIN_HOUR || now.tm_hour < NIGHT_END_HOUR;
    const bool idle = lv_display_get_inactive_time(NULL) > NIGHT_DIM_INACTIVE_MS;
    panel_set_brightness(night && idle ? BRIGHTNESS_NIGHT : BRIGHTNESS_ACTIVE);
}

void app_main(void) {
    lcd_vci_en_init();

    // 先初始化触摸并扫描 I2C：能扫到 0x58 说明排线、3V3 与触摸复位都正常。
    // DSI 读 ID 在面板不应答时会无限等待（IDF 的读 FIFO 循环没有超时），放在后面便于定位。
    ESP_ERROR_CHECK(app_touch_init());
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_lvgl_init());

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    ESP_ERROR_CHECK(flip_clock_start(lvgl_disp));
    lv_timer_create(brightness_policy_cb, 1000, NULL);
    esp_lv_adapter_unlock();

    // 串口性能遥测（menuconfig 的 "Performance probe" 可关）。翻页钟静置时只有
    // 冒号每秒闪一下，redraw/motion FPS 约 1 是正常的；整分翻页时上升。
    ESP_ERROR_CHECK(perf_probe_start(lvgl_disp));
}
