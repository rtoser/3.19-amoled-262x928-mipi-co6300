/*
 * LVGL 性能探针（从 piconetmon time-center 的 perf_probe 裁剪移植）。
 *
 * 挂到任意 lv_display_t 上，按窗口周期在串口输出三行遥测：
 *   [FRAME] 真实 redraw/motion FPS + refresh/render/draw 估计/flush/flush-wait
 *           的 avg/p50/p95/max（微秒）
 *   [CPU]   跨核真实 idle%（esp_ipc 同步 + 代际校验）、堆与 LVGL 任务栈水位
 *   [ERR]   帧超时 / 归因失败 / 被放弃的帧 / 样本溢出累计
 *
 * 与 sysmon 叠加层不同：refresh 不等于呈现新画面。redraw FPS 只统计真的渲染
 * 了内容的刷新；motion FPS 只统计应用通过 perf_probe_note_motion_step() 标记
 * 过内容变化的重绘帧——衡量动画流畅度时以 motion FPS 为准。
 */
#ifndef PERF_PROBE_H
#define PERF_PROBE_H

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 挂接探针并启动上报任务。每个固件只支持一个 display。 */
esp_err_t perf_probe_start(lv_display_t *display);

/* 停止上报任务（事件回调保留但不再累计）。 */
void perf_probe_stop(void);

/* 应用在真实内容变化（动画步进、数据更新）时调用一次；
 * 下一个完成的重绘帧计为一个 motion 帧。 */
void perf_probe_note_motion_step(void);

/* 可选：标记当前场景（出现在遥测行的 mode= 字段），同时重置统计窗口
 * 与 idle 基准，避免把上一场景的时间混进来。label 最多保留 15 字符。 */
void perf_probe_set_label(const char *label);

#ifdef __cplusplus
}
#endif

#endif
