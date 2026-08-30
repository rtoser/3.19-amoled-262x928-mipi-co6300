/**
 * @file flip_clock.h
 * @brief 机械翻页时钟界面：HH:MM 四张翻页卡 + 冒号秒闪。
 *
 * 需要横屏逻辑分辨率（宽 ≥ 708）；时区固定 UTC+8，系统时间未校准时
 * 用一个固定的默认时间起走（接入 C5 hosted 的 NTP 后自然被真实时间取代）。
 */
#ifndef FLIP_CLOCK_H
#define FLIP_CLOCK_H

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在当前活动屏幕上创建翻页钟并启动走时。必须在持有 LVGL 锁时调用。 */
esp_err_t flip_clock_start(lv_display_t *display);

#ifdef __cplusplus
}
#endif

#endif /* FLIP_CLOCK_H */
