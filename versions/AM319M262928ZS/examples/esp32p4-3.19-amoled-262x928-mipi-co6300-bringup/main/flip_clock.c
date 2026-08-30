#include "flip_clock.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "flip_digit_physics.h"
#include "lv_font_montserrat_bold_digits_150.h"
#include "perf_probe.h"

/* HH:MM:SS 六张翻页卡。卡面 120×156 配 150px 数字字体（与 time-center 的
 * 152×200 / 192px 冻结几何同比例缩小，装进 928px 逻辑横屏）：
 * 一行 = 3 对 × (120+16+120) + 2 组冒号 × (24+20+24) = 768 + 136 = 904px。 */
#define CLOCK_DIGIT_COUNT   6
#define CLOCK_CARD_WIDTH    120
#define CLOCK_CARD_HEIGHT   156
#define CLOCK_CARD_RADIUS   10
#define CLOCK_DIGIT_GAP     16
#define CLOCK_COLON_WIDTH   20
#define CLOCK_COLON_GAP     24
#define CLOCK_ROW_WIDTH     (CLOCK_DIGIT_COUNT * CLOCK_CARD_WIDTH +   \
                             3 * CLOCK_DIGIT_GAP +                    \
                             2 * (2 * CLOCK_COLON_GAP + CLOCK_COLON_WIDTH))
#define CLOCK_ROW_HEIGHT    (CLOCK_CARD_HEIGHT + 12)
#define CLOCK_TICK_MS       70
/* 级联翻页：同一时刻只有一张卡在翻（单卡逐帧光栅化 p95 约 8.7 ms，两卡并发
 * 即 48 FPS、三卡 30 FPS）。翻片 260 ms + 起翻间隔 280 ms → 并发恒为 1。 */
#define CLOCK_FLIP_SPACING_MS 280
/* 系统时间早于 2025 年视为未校准，从一个固定默认时间起走。 */
#define CLOCK_EPOCH_FLOOR   1735689600 /* 2025-01-01 00:00:00 UTC */
#define CLOCK_DEFAULT_TIME  1788098370 /* 2026-08-30 21:59:30 UTC+8（半分钟后可现场验证夜间降亮） */

_Static_assert(CLOCK_CARD_WIDTH <= FLIP_DIGIT_PHYSICS_MAX_CARD_W &&
               CLOCK_CARD_HEIGHT <= FLIP_DIGIT_PHYSICS_MAX_CARD_H,
               "卡面几何不能超过组件的静态缓冲上限");

static const char *TAG = "flip_clock";

static flip_digit_physics_t s_digits[CLOCK_DIGIT_COUNT];
static flip_digit_physics_canvas_t *s_canvases;
static flip_digit_physics_atlas_t *s_atlas;
static flip_digit_physics_frame_atlas_t s_frame_atlas;
static flip_digit_physics_scratch_t *s_scratch;
static lv_obj_t *s_colons[2];
static lv_timer_t *s_timer;
static int s_shown_second = -1;
static char s_applied[CLOCK_DIGIT_COUNT];   /* 已下发到卡面的目标值 */
static char s_pending[CLOCK_DIGIT_COUNT];   /* 待级联下发的目标值（0 = 无） */
static uint32_t s_last_flip_tick;

static lv_obj_t *create_plain_object(lv_obj_t *parent, int32_t x, int32_t y,
                                     int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static esp_err_t create_colon(lv_obj_t *parent, int32_t x, size_t slot)
{
    lv_obj_t *colon = create_plain_object(parent, x, 0, CLOCK_COLON_WIDTH,
                                          CLOCK_CARD_HEIGHT);
    if (colon == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_colons[slot] = colon;
    const int32_t dot_size = 12;
    const int32_t dot_x = (CLOCK_COLON_WIDTH - dot_size) / 2;
    const int32_t dot_y[] = {
        CLOCK_CARD_HEIGHT * 36 / 100,
        CLOCK_CARD_HEIGHT * 67 / 100,
    };
    for (size_t i = 0; i < 2; i++) {
        lv_obj_t *dot = create_plain_object(colon, dot_x, dot_y[i], dot_size, dot_size);
        if (dot == NULL) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x5A5A5A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(dot, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(dot, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(dot, 4, LV_PART_MAIN);
        lv_obj_set_style_shadow_offset_y(dot, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(dot, LV_OPA_40, LV_PART_MAIN);
    }
    return ESP_OK;
}

static void time_to_digits(const struct tm *now, char out[CLOCK_DIGIT_COUNT])
{
    out[0] = (char)('0' + now->tm_hour / 10);
    out[1] = (char)('0' + now->tm_hour % 10);
    out[2] = (char)('0' + now->tm_min / 10);
    out[3] = (char)('0' + now->tm_min % 10);
    out[4] = (char)('0' + now->tm_sec / 10);
    out[5] = (char)('0' + now->tm_sec % 10);
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    time_t now_ts = time(NULL);
    struct tm now;
    localtime_r(&now_ts, &now);

    if (now.tm_sec != s_shown_second) {
        s_shown_second = now.tm_sec;
        /* 跳变的位进入级联队列（覆盖旧目标），不在此处直接起翻。 */
        char digits[CLOCK_DIGIT_COUNT];
        time_to_digits(&now, digits);
        for (size_t i = 0; i < CLOCK_DIGIT_COUNT; i++) {
            if (digits[i] != s_applied[i]) {
                s_pending[i] = digits[i];
            }
        }
        const lv_opa_t colon_opa = (now.tm_sec & 1) ? LV_OPA_30 : LV_OPA_COVER;
        for (size_t i = 0; i < 2; i++) {
            lv_obj_set_style_opa(s_colons[i], colon_opa, LV_PART_MAIN);
        }
        perf_probe_note_motion_step();
    }

    /* 级联出队：每 280 ms 最多起翻一张卡，从最低位（秒个位）向高位推进，
     * 进位表现为一串波浪式翻页——和机械翻页钟的联动方式一致。 */
    if (lv_tick_elaps(s_last_flip_tick) < CLOCK_FLIP_SPACING_MS) {
        return;
    }
    for (int i = CLOCK_DIGIT_COUNT - 1; i >= 0; i--) {
        if (s_pending[i] != 0) {
            flip_digit_physics_set_value(&s_digits[i], s_pending[i], LV_ANIM_ON);
            s_applied[i] = s_pending[i];
            s_pending[i] = 0;
            s_last_flip_tick = lv_tick_get();
            break;
        }
    }
}

esp_err_t flip_clock_start(lv_display_t *display)
{
    const int32_t hor_res = lv_display_get_horizontal_resolution(display);
    const int32_t ver_res = lv_display_get_vertical_resolution(display);
    if (hor_res < CLOCK_ROW_WIDTH || ver_res < CLOCK_CARD_HEIGHT) {
        ESP_LOGE(TAG, "screen %dx%d too small for the %dpx clock row (need landscape)",
                 (int)hor_res, (int)ver_res, (int)CLOCK_ROW_WIDTH);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 未校准的系统时间从固定默认值起走；C5 NTP 接入后 settimeofday 会覆盖。 */
    setenv("TZ", "CST-8", 1);
    tzset();
    if (time(NULL) < CLOCK_EPOCH_FLOOR) {
        const struct timeval tv = {.tv_sec = CLOCK_DEFAULT_TIME};
        settimeofday(&tv, NULL);
    }

    s_canvases = heap_caps_calloc(CLOCK_DIGIT_COUNT, sizeof(*s_canvases),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scratch = heap_caps_calloc(1, sizeof(*s_scratch),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_atlas = heap_caps_calloc(1, sizeof(*s_atlas),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_canvases == NULL || s_scratch == NULL || s_atlas == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x05070D), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x151A25), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_HOR, LV_PART_MAIN);

    lv_obj_t *row = create_plain_object(screen,
                                        (hor_res - CLOCK_ROW_WIDTH) / 2,
                                        (ver_res - CLOCK_ROW_HEIGHT) / 2,
                                        CLOCK_ROW_WIDTH, CLOCK_ROW_HEIGHT);
    if (row == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    time_t now_ts = time(NULL);
    struct tm now;
    localtime_r(&now_ts, &now);
    char initial[CLOCK_DIGIT_COUNT];
    time_to_digits(&now, initial);

    const flip_digit_geometry_t geometry = {
        .width = CLOCK_CARD_WIDTH,
        .height = CLOCK_CARD_HEIGHT,
        .radius = CLOCK_CARD_RADIUS,
        .font = &lv_font_montserrat_bold_digits_150,
    };

    int32_t x = 0;
    size_t colon_slot = 0;
    for (size_t i = 0; i < CLOCK_DIGIT_COUNT; i++) {
        if (!flip_digit_physics_init(&s_digits[i], &s_canvases[i], s_atlas,
                                     &s_frame_atlas, s_scratch, NULL, NULL,
                                     row, initial[i], &geometry)) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_pos(flip_digit_physics_get_obj(&s_digits[i]), x, 0);
        x += CLOCK_CARD_WIDTH;
        if (i % 2 == 0) {
            x += CLOCK_DIGIT_GAP;         /* 对内间距 */
        } else if (i < CLOCK_DIGIT_COUNT - 1) {
            x += CLOCK_COLON_GAP;         /* 对间冒号组 */
            const esp_err_t err = create_colon(row, x, colon_slot++);
            if (err != ESP_OK) {
                return err;
            }
            x += CLOCK_COLON_WIDTH + CLOCK_COLON_GAP;
        }
    }

    s_shown_second = now.tm_sec;
    memcpy(s_applied, initial, sizeof(s_applied));
    memset(s_pending, 0, sizeof(s_pending));
    s_last_flip_tick = lv_tick_get();
    s_timer = lv_timer_create(tick_cb, CLOCK_TICK_MS, NULL);
    if (s_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    perf_probe_set_label("flip_clock");
    ESP_LOGI(TAG, "flip clock started at %02d:%02d:%02d (row %dx%d @ %dx%d)",
             now.tm_hour, now.tm_min, now.tm_sec, (int)CLOCK_ROW_WIDTH,
             (int)CLOCK_ROW_HEIGHT, (int)hor_res, (int)ver_res);
    return ESP_OK;
}
