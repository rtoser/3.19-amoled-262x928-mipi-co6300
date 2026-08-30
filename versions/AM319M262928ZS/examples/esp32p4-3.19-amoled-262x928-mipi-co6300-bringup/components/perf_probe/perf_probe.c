#include "perf_probe.h"

#include "sdkconfig.h"

#if CONFIG_PERF_PROBE_ENABLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PERF_WINDOW_MS             ((uint32_t)CONFIG_PERF_PROBE_WINDOW_MS)
#define PERF_STOP_POLL_MS          100U
#define PERF_FRAME_WAIT_TIMEOUT_MS 250U
#define PERF_SAMPLE_CAPACITY       128U
#define PERF_TASK_STACK_BYTES      6144U
#define PERF_TASK_PRIORITY         1U
#define PERF_LABEL_CAPACITY        16U

static const char *TAG = "perf_probe";

/* 帧内阶段。draw 估计 = render(含 flush) − flush，flush-wait 是等待显示驱动
 * 收下缓冲的时间（direct/VSYNC 模式下主要是等翻页）。 */
typedef enum {
    PERF_METRIC_REFRESH = 0,
    PERF_METRIC_RENDER_INCLUSIVE,
    PERF_METRIC_DRAW_ESTIMATE,
    PERF_METRIC_FLUSH,
    PERF_METRIC_FLUSH_WAIT,
    PERF_METRIC_COUNT,
} perf_metric_id_t;

typedef struct {
    uint64_t total_us;
    uint32_t count;
    uint32_t maximum_us;
    uint32_t samples[PERF_SAMPLE_CAPACITY];
    uint16_t stored;
    uint32_t overflow;
} perf_metric_window_t;

typedef struct {
    perf_metric_window_t metrics[PERF_METRIC_COUNT];
    uint32_t refresh_count;
    uint32_t redraw_count;
    uint32_t motion_count;
    uint32_t flush_count;
    uint32_t abandoned_frame_count;
    uint64_t dirty_pixels;
    int64_t start_us;
    uint32_t idle_generation;
    char label[PERF_LABEL_CAPACITY];
} perf_window_t;

typedef struct {
    bool active;
    bool redraw;
    int64_t refresh_start_us;
    int64_t render_start_us;
    int64_t flush_start_us;
    int64_t flush_wait_start_us;
    uint32_t render_inclusive_us;
    uint32_t flush_us;
    uint32_t flush_wait_us;
    uint32_t flush_count;
    uint64_t dirty_pixels;
} perf_active_frame_t;

typedef struct {
    uint32_t count;
    uint32_t average_us;
    uint32_t p50_us;
    uint32_t p95_us;
    uint32_t maximum_us;
} perf_metric_summary_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static perf_window_t s_windows[2];
static perf_active_frame_t s_frame;
static uint32_t s_frame_timeout_total;
static uint32_t s_attribution_error_total;
static uint32_t s_motion_epoch;
static uint32_t s_last_motion_epoch;
static char s_label[PERF_LABEL_CAPACITY] = "app";
static uint32_t s_idle_generation;
static uint32_t s_idle_report_generation;
static int64_t s_idle_timestamp_us;
#if configGENERATE_RUN_TIME_STATS
static configRUN_TIME_COUNTER_TYPE s_idle_previous[configNUMBER_OF_CORES];
#endif
static TaskHandle_t s_task;
static lv_display_t *s_display;
static uint8_t s_active_window;
static uint8_t s_snapshot_window;
static int64_t s_snapshot_end_us;
static bool s_snapshot_requested;
static bool s_snapshot_ready;
static bool s_started;

/* ---------------------------------------------------------------- 工具 --- */

static void idle_counter_sync_cb(void *argument)
{
    (void)argument;
}

/* 让另一核在非临界区提交最新 idle 计数（time-center 教训：
 * 绝不能在 critical section 里做跨核 IPC）。 */
static bool sync_idle_counters(void)
{
    const BaseType_t current_core = xPortGetCoreID();
    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; core++) {
        if (core == current_core) {
            continue;
        }
        if (esp_ipc_call_blocking((uint32_t)core, idle_counter_sync_cb, NULL) != ESP_OK) {
            return false;
        }
    }
    return true;
}

static int compare_u32(const void *left, const void *right)
{
    const uint32_t l = *(const uint32_t *)left;
    const uint32_t r = *(const uint32_t *)right;
    return (l > r) - (l < r);
}

static uint32_t duration_to_u32(int64_t start_us, int64_t end_us)
{
    if (start_us <= 0 || end_us < start_us) {
        return 0;
    }
    const uint64_t duration_us = (uint64_t)(end_us - start_us);
    return duration_us > UINT32_MAX ? UINT32_MAX : (uint32_t)duration_us;
}

static void metric_record_locked(perf_metric_window_t *metric, uint32_t duration_us)
{
    metric->count++;
    metric->total_us += duration_us;
    if (duration_us > metric->maximum_us) {
        metric->maximum_us = duration_us;
    }
    if (metric->stored < PERF_SAMPLE_CAPACITY) {
        metric->samples[metric->stored++] = duration_us;
    } else {
        metric->overflow++;
    }
}

static perf_metric_summary_t metric_summarize(perf_metric_window_t *metric)
{
    perf_metric_summary_t summary = {0};
    summary.count = metric->count;
    summary.maximum_us = metric->maximum_us;
    if (metric->count > 0) {
        summary.average_us = (uint32_t)(metric->total_us / metric->count);
    }
    if (metric->stored > 0) {
        qsort(metric->samples, metric->stored, sizeof(metric->samples[0]), compare_u32);
        summary.p50_us = metric->samples[(metric->stored - 1U) * 50U / 100U];
        summary.p95_us = metric->samples[(metric->stored - 1U) * 95U / 100U];
    }
    return summary;
}

/* ---------------------------------------------------------- 窗口与帧聚合 --- */

static void window_begin_locked(perf_window_t *window, int64_t now_us)
{
    window->start_us = now_us;
    window->idle_generation = s_idle_generation;
    strlcpy(window->label, s_label, sizeof(window->label));
}

static void window_reset_locked(perf_window_t *window, int64_t now_us)
{
    memset(window, 0, sizeof(*window));
    window_begin_locked(window, now_us);
}

static void snapshot_rotate_locked(int64_t now_us)
{
    s_snapshot_window = s_active_window;
    s_active_window ^= 1U;
    window_begin_locked(&s_windows[s_active_window], now_us);
    s_snapshot_end_us = now_us;
    s_snapshot_requested = false;
    s_snapshot_ready = true;
}

static void frame_finish_locked(int64_t now_us)
{
    if (!s_frame.active) {
        return;
    }

    perf_window_t *window = &s_windows[s_active_window];
    metric_record_locked(&window->metrics[PERF_METRIC_REFRESH],
                         duration_to_u32(s_frame.refresh_start_us, now_us));
    window->refresh_count++;

    if (s_frame.redraw) {
        /* RENDER_START→READY 区间包含其中的 flush；两者矛盾说明事件序列
         * 被打断（如错过事件），该帧不参与 motion 统计。 */
        const bool attribution_invalid = s_frame.render_inclusive_us < s_frame.flush_us;
        if (attribution_invalid) {
            s_attribution_error_total++;
        }
        const uint32_t draw_estimate_us =
            attribution_invalid ? 0 : s_frame.render_inclusive_us - s_frame.flush_us;

        metric_record_locked(&window->metrics[PERF_METRIC_RENDER_INCLUSIVE],
                             s_frame.render_inclusive_us);
        metric_record_locked(&window->metrics[PERF_METRIC_DRAW_ESTIMATE], draw_estimate_us);
        metric_record_locked(&window->metrics[PERF_METRIC_FLUSH], s_frame.flush_us);
        metric_record_locked(&window->metrics[PERF_METRIC_FLUSH_WAIT], s_frame.flush_wait_us);

        window->redraw_count++;
        window->flush_count += s_frame.flush_count;
        window->dirty_pixels += s_frame.dirty_pixels;

        if (s_motion_epoch != s_last_motion_epoch) {
            if (!attribution_invalid) {
                window->motion_count++;
            }
            s_last_motion_epoch = s_motion_epoch;
        }
    }

    memset(&s_frame, 0, sizeof(s_frame));
}

static void display_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    const int64_t now_us = esp_timer_get_time();
    const lv_area_t *area = lv_event_get_param(event);

    taskENTER_CRITICAL(&s_lock);
    if (!s_started) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    switch (code) {
    case LV_EVENT_REFR_START:
        if (s_frame.active) {
            s_windows[s_active_window].abandoned_frame_count++;
        }
        memset(&s_frame, 0, sizeof(s_frame));
        s_frame.active = true;
        s_frame.refresh_start_us = now_us;
        break;
    case LV_EVENT_REFR_READY:
        frame_finish_locked(now_us);
        if (s_snapshot_requested) {
            snapshot_rotate_locked(now_us);
        }
        break;
    case LV_EVENT_RENDER_START:
        if (s_frame.active) {
            s_frame.redraw = true;
            s_frame.render_start_us = now_us;
        }
        break;
    case LV_EVENT_RENDER_READY:
        if (s_frame.active && s_frame.render_start_us > 0) {
            s_frame.render_inclusive_us = duration_to_u32(s_frame.render_start_us, now_us);
        }
        break;
    case LV_EVENT_FLUSH_START:
        if (s_frame.active) {
            s_frame.flush_start_us = now_us;
            s_frame.flush_count++;
            if (area != NULL) {
                s_frame.dirty_pixels += (uint64_t)lv_area_get_size(area);
            }
        }
        break;
    case LV_EVENT_FLUSH_FINISH:
        if (s_frame.active && s_frame.flush_start_us > 0) {
            s_frame.flush_us += duration_to_u32(s_frame.flush_start_us, now_us);
            s_frame.flush_start_us = 0;
        }
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        if (s_frame.active) {
            s_frame.flush_wait_start_us = now_us;
        }
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        if (s_frame.active && s_frame.flush_wait_start_us > 0) {
            s_frame.flush_wait_us += duration_to_u32(s_frame.flush_wait_start_us, now_us);
            s_frame.flush_wait_start_us = 0;
        }
        break;
    default:
        break;
    }
    taskEXIT_CRITICAL(&s_lock);
}

/* ---------------------------------------------------------------- 上报 --- */

static UBaseType_t lvgl_task_stack_watermark(void)
{
    /* esp_lvgl_port 的任务叫 "taskLVGL"，esp_lvgl_adapter 的叫 "lvgl"。 */
    static const char *const candidates[] = {"taskLVGL", "lvgl", "LVGL"};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        TaskHandle_t task = xTaskGetHandle(candidates[i]);
        if (task != NULL) {
            return uxTaskGetStackHighWaterMark(task);
        }
    }
    return 0;
}

static void perf_task(void *argument)
{
    (void)argument;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        for (uint32_t elapsed_ms = 0; elapsed_ms < PERF_WINDOW_MS;
             elapsed_ms += PERF_STOP_POLL_MS) {
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PERF_STOP_POLL_MS));
            taskENTER_CRITICAL(&s_lock);
            const bool running = s_started;
            taskEXIT_CRITICAL(&s_lock);
            if (!running) {
                goto task_exit;
            }
        }

        const bool idle_sync_ok = sync_idle_counters();
#if configGENERATE_RUN_TIME_STATS
        configRUN_TIME_COUNTER_TYPE idle_now[configNUMBER_OF_CORES];
        for (BaseType_t core = 0; core < configNUMBER_OF_CORES; core++) {
            idle_now[core] = ulTaskGetIdleRunTimeCounterForCore(core);
        }
#endif
        const int64_t idle_sample_us = esp_timer_get_time();

        /* 请求在帧边界轮换窗口；帧长时间不收口则记 frame_timeout 并继续等。 */
        const int64_t wait_start_us = esp_timer_get_time();
        bool stop_requested = false;
        bool frame_timeout_reported = false;
        uint8_t snapshot_index = 0;
        int64_t now_us = 0;

        taskENTER_CRITICAL(&s_lock);
        if (!s_started) {
            stop_requested = true;
        } else if (!s_frame.active) {
            snapshot_rotate_locked(wait_start_us);
        } else {
            s_snapshot_requested = true;
        }
        taskEXIT_CRITICAL(&s_lock);

        while (!stop_requested) {
            bool swapped = false;
            const int64_t poll_us = esp_timer_get_time();
            taskENTER_CRITICAL(&s_lock);
            if (!s_started) {
                stop_requested = true;
            } else if (s_snapshot_ready) {
                snapshot_index = s_snapshot_window;
                now_us = s_snapshot_end_us;
                s_snapshot_ready = false;
                swapped = true;
            } else if (!frame_timeout_reported &&
                       poll_us - wait_start_us >=
                           (int64_t)PERF_FRAME_WAIT_TIMEOUT_MS * 1000) {
                s_frame_timeout_total++;
                frame_timeout_reported = true;
            }
            taskEXIT_CRITICAL(&s_lock);
            if (swapped) {
                break;
            }
            if (!stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        if (stop_requested) {
            break;
        }

        perf_window_t *snapshot = &s_windows[snapshot_index];
        const uint32_t window_us = duration_to_u32(snapshot->start_us, now_us);
        const uint32_t window_ms = window_us / 1000U;

        const perf_metric_summary_t refresh =
            metric_summarize(&snapshot->metrics[PERF_METRIC_REFRESH]);
        const perf_metric_summary_t render =
            metric_summarize(&snapshot->metrics[PERF_METRIC_RENDER_INCLUSIVE]);
        const perf_metric_summary_t draw =
            metric_summarize(&snapshot->metrics[PERF_METRIC_DRAW_ESTIMATE]);
        const perf_metric_summary_t flush =
            metric_summarize(&snapshot->metrics[PERF_METRIC_FLUSH]);
        const perf_metric_summary_t flush_wait =
            metric_summarize(&snapshot->metrics[PERF_METRIC_FLUSH_WAIT]);

        uint32_t sample_overflow = 0;
        for (size_t i = 0; i < PERF_METRIC_COUNT; i++) {
            sample_overflow += snapshot->metrics[i].overflow;
        }

        /* idle：窗口切换（set_label）后的第一个窗口只重建基准，标 reset。 */
        uint32_t idle_percent = 0;
        uint32_t idle_core_percent[configNUMBER_OF_CORES] = {0};
        bool idle_valid = false;
#if configGENERATE_RUN_TIME_STATS
        idle_valid = idle_sync_ok &&
                     snapshot->idle_generation == s_idle_report_generation;
        const uint64_t elapsed_us =
            idle_valid && s_idle_timestamp_us > 0 && idle_sample_us > s_idle_timestamp_us
                ? (uint64_t)(idle_sample_us - s_idle_timestamp_us)
                : 0;
        uint64_t idle_delta = 0;
        for (BaseType_t core = 0; core < configNUMBER_OF_CORES; core++) {
            const configRUN_TIME_COUNTER_TYPE core_delta =
                idle_now[core] - s_idle_previous[core];
            idle_delta += core_delta;
            idle_core_percent[core] =
                elapsed_us > 0 ? (uint32_t)(((uint64_t)core_delta * 100U) / elapsed_us) : 0;
            if (idle_core_percent[core] > 100U) {
                idle_core_percent[core] = 100U;
            }
            s_idle_previous[core] = idle_now[core];
        }
        const uint64_t idle_capacity_us = elapsed_us * configNUMBER_OF_CORES;
        idle_percent =
            idle_capacity_us > 0 ? (uint32_t)((idle_delta * 100U) / idle_capacity_us) : 0;
        if (idle_percent > 100U) {
            idle_percent = 100U;
        }
        s_idle_timestamp_us = idle_sample_us;
        s_idle_report_generation = snapshot->idle_generation;
#else
        (void)idle_sync_ok;
#endif

        const uint32_t redraw_fps_x10 =
            window_us > 0
                ? (uint32_t)(((uint64_t)snapshot->redraw_count * 10000000ULL) / window_us)
                : 0;
        const uint32_t motion_fps_x10 =
            window_us > 0
                ? (uint32_t)(((uint64_t)snapshot->motion_count * 10000000ULL) / window_us)
                : 0;

        const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

        printf("I (%lu) %s: [FRAME] mode=%s win=%ums redraw_fps=%u.%u motion_fps=%u.%u "
               "refresh=%u redraw=%u motion=%u dirty_px=%llu "
               "refresh_us=%u/%u/%u/%u render_us=%u/%u/%u/%u draw_us=%u/%u/%u/%u "
               "flush_us=%u/%u/%u/%u wait_us=%u/%u/%u/%u\n",
               (unsigned long)(esp_timer_get_time() / 1000), TAG,
               snapshot->label, (unsigned)window_ms,
               (unsigned)(redraw_fps_x10 / 10U), (unsigned)(redraw_fps_x10 % 10U),
               (unsigned)(motion_fps_x10 / 10U), (unsigned)(motion_fps_x10 % 10U),
               (unsigned)snapshot->refresh_count, (unsigned)snapshot->redraw_count,
               (unsigned)snapshot->motion_count,
               (unsigned long long)snapshot->dirty_pixels,
               (unsigned)refresh.average_us, (unsigned)refresh.p50_us,
               (unsigned)refresh.p95_us, (unsigned)refresh.maximum_us,
               (unsigned)render.average_us, (unsigned)render.p50_us,
               (unsigned)render.p95_us, (unsigned)render.maximum_us,
               (unsigned)draw.average_us, (unsigned)draw.p50_us,
               (unsigned)draw.p95_us, (unsigned)draw.maximum_us,
               (unsigned)flush.average_us, (unsigned)flush.p50_us,
               (unsigned)flush.p95_us, (unsigned)flush.maximum_us,
               (unsigned)flush_wait.average_us, (unsigned)flush_wait.p50_us,
               (unsigned)flush_wait.p95_us, (unsigned)flush_wait.maximum_us);
        printf("I (%lu) %s: [CPU] mode=%s idle=%u%% core=%u/%u%% idle_valid=%s "
               "internal=%u/%u/%uB psram=%u/%u/%uB lvgl_stack=%uB\n",
               (unsigned long)(esp_timer_get_time() / 1000), TAG,
               snapshot->label, (unsigned)idle_percent,
               (unsigned)idle_core_percent[0],
               (unsigned)(configNUMBER_OF_CORES > 1 ? idle_core_percent[1] : 0),
#if configGENERATE_RUN_TIME_STATS
               idle_valid ? "valid" : "reset",
#else
               "unsupported",
#endif
               (unsigned)heap_caps_get_free_size(internal_caps),
               (unsigned)heap_caps_get_minimum_free_size(internal_caps),
               (unsigned)heap_caps_get_largest_free_block(internal_caps),
               (unsigned)heap_caps_get_free_size(psram_caps),
               (unsigned)heap_caps_get_minimum_free_size(psram_caps),
               (unsigned)heap_caps_get_largest_free_block(psram_caps),
               (unsigned)lvgl_task_stack_watermark());
        taskENTER_CRITICAL(&s_lock);
        const uint32_t frame_timeouts = s_frame_timeout_total;
        const uint32_t attribution_errors = s_attribution_error_total;
        taskEXIT_CRITICAL(&s_lock);
        printf("I (%lu) %s: [ERR] frame_timeout=%u attribution=%u abandoned=%u "
               "sample_overflow=%u\n",
               (unsigned long)(esp_timer_get_time() / 1000), TAG,
               (unsigned)frame_timeouts, (unsigned)attribution_errors,
               (unsigned)snapshot->abandoned_frame_count, (unsigned)sample_overflow);

        memset(snapshot, 0, sizeof(*snapshot));
        last_wake = xTaskGetTickCount();
    }

task_exit:
    taskENTER_CRITICAL(&s_lock);
    s_task = NULL;
    taskEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- 公共 API --- */

esp_err_t perf_probe_start(lv_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    const bool already = s_started;
    taskEXIT_CRITICAL(&s_lock);
    if (already) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    memset(s_windows, 0, sizeof(s_windows));
    memset(&s_frame, 0, sizeof(s_frame));
    s_active_window = 0;
    s_snapshot_requested = false;
    s_snapshot_ready = false;
    s_idle_generation++;
    window_begin_locked(&s_windows[s_active_window], now_us);
    s_started = true;
    taskEXIT_CRITICAL(&s_lock);

    if (s_display == NULL) {
        lv_display_add_event_cb(display, display_event_cb, LV_EVENT_ALL, NULL);
        s_display = display;
    }

    if (xTaskCreate(perf_task, "perf_probe", PERF_TASK_STACK_BYTES / sizeof(StackType_t),
                    NULL, PERF_TASK_PRIORITY, &s_task) != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        s_started = false;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "report task creation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "attached, window=%ums", (unsigned)PERF_WINDOW_MS);
    return ESP_OK;
}

void perf_probe_stop(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_started = false;
    taskEXIT_CRITICAL(&s_lock);
    while (true) {
        taskENTER_CRITICAL(&s_lock);
        const bool alive = s_task != NULL;
        taskEXIT_CRITICAL(&s_lock);
        if (!alive) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PERF_STOP_POLL_MS));
    }
}

void perf_probe_note_motion_step(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_motion_epoch++;
    taskEXIT_CRITICAL(&s_lock);
}

void perf_probe_set_label(const char *label)
{
    if (label == NULL || label[0] == '\0') {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_label, label, sizeof(s_label));
    s_idle_generation++;
    memset(&s_frame, 0, sizeof(s_frame));
    s_last_motion_epoch = s_motion_epoch;
    window_reset_locked(&s_windows[s_active_window], now_us);
    taskEXIT_CRITICAL(&s_lock);
}

#else /* !CONFIG_PERF_PROBE_ENABLE */

esp_err_t perf_probe_start(lv_display_t *display)
{
    (void)display;
    return ESP_OK;
}

void perf_probe_stop(void) {}
void perf_probe_note_motion_step(void) {}
void perf_probe_set_label(const char *label)
{
    (void)label;
}

#endif /* CONFIG_PERF_PROBE_ENABLE */
