/**
 * @file flip_digit_physics.c
 * @brief Fixed-point scanline renderer for a perspective flip-clock flap.
 */

#include "flip_digit_physics.h"

#include <string.h>

#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"

/* 时间轴随 ANIMATION_MS 520→260 整体 1/2 缩放（见 flip_digit_physics.h 的产品调优注释） */
#define PHYSICS_STATIC_START_END_MS   10
#define PHYSICS_CANVAS_START_END_MS   20
#define PHYSICS_FLIP_END_MS           FLIP_DIGIT_PHYSICS_CONTACT_MS
#define PHYSICS_REBOUND_HALF_MS       18
#define PHYSICS_REBOUND_APEX_MS       \
    (PHYSICS_FLIP_END_MS + PHYSICS_REBOUND_HALF_MS)
#define PHYSICS_SETTLE_END_MS         \
    (PHYSICS_REBOUND_APEX_MS + PHYSICS_REBOUND_HALF_MS)
#define PHYSICS_CANVAS_END_MS         241
#define PHYSICS_REBOUND_DEG           18
#define PHYSICS_PROGRESS_MAX          1024
#define PHYSICS_CARD_CONTENT_OFFSET   1

_Static_assert(PHYSICS_STATIC_START_END_MS < PHYSICS_CANVAS_START_END_MS &&
               PHYSICS_CANVAS_START_END_MS < PHYSICS_FLIP_END_MS &&
               PHYSICS_FLIP_END_MS < PHYSICS_REBOUND_APEX_MS &&
               PHYSICS_REBOUND_APEX_MS < PHYSICS_SETTLE_END_MS &&
               PHYSICS_SETTLE_END_MS < PHYSICS_CANVAS_END_MS &&
               PHYSICS_CANVAS_END_MS <= FLIP_DIGIT_PHYSICS_ANIMATION_MS,
               "physics timeline must be monotonic");

static const char *const digit_text[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};

static lv_color16a_t *render_pixels(flip_digit_physics_t *digit)
{
    if (digit->render_target_override != NULL) {
        return digit->render_target_override;
    }
#if FLIP_DIGIT_STAGED_RENDER
    return digit->scratch->render_pixels;
#else
    return digit->canvas_storage->canvas_pixels;
#endif
}

static uint32_t elapsed_us(int64_t start_us, int64_t end_us)
{
    if (start_us <= 0 || end_us < start_us) {
        return 0;
    }
    const uint64_t duration_us = (uint64_t)(end_us - start_us);
    return duration_us > UINT32_MAX ? UINT32_MAX : (uint32_t)duration_us;
}

static lv_obj_t *create_root(lv_obj_t *parent,
                             int16_t width,
                             int16_t height)
{
    lv_obj_t *root = lv_obj_create(parent);
    if (root == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, width, height);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return root;
}

static int32_t normalized_progress(int32_t elapsed, int32_t duration)
{
    if (elapsed <= 0) return 0;
    if (elapsed >= duration) return PHYSICS_PROGRESS_MAX;
    return elapsed * PHYSICS_PROGRESS_MAX / duration;
}

static int32_t smoothstep_progress(int32_t elapsed, int32_t duration)
{
    int32_t progress = normalized_progress(elapsed, duration);
    return progress * progress *
           (3 * PHYSICS_PROGRESS_MAX - 2 * progress) /
           (PHYSICS_PROGRESS_MAX * PHYSICS_PROGRESS_MAX);
}

static int32_t interpolate_progress(int32_t start,
                                    int32_t end,
                                    int32_t progress)
{
    return start + (end - start) * progress / PHYSICS_PROGRESS_MAX;
}

static void set_static_split(flip_digit_physics_t *digit,
                             char top_digit,
                             char bottom_digit)
{
    lv_label_set_text_static(digit->base.top_label,
                             digit_text[top_digit - '0']);
    lv_label_set_text_static(digit->base.bottom_label,
                             digit_text[bottom_digit - '0']);
}

static bool build_half_mask(flip_digit_physics_t *digit,
                            char value,
                            bool bottom,
                            uint8_t *mask)
{
    const lv_font_t *font = digit->geometry.font;
    int16_t face_width = digit->geometry.width - 2;
    int16_t face_height = digit->geometry.height / 2 - 2;
    size_t mask_size = (size_t)face_width * face_height;
    memset(mask, 0, mask_size);

    lv_font_glyph_dsc_t glyph;
    if (!lv_font_get_glyph_dsc(font, &glyph, (uint32_t)value, 0)) {
        return false;
    }

    lv_draw_buf_t glyph_buffer;
    void *glyph_scratch = render_pixels(digit);
    uint32_t scratch_size =
        sizeof(digit->canvas_storage->canvas_pixels);
    if (lv_draw_buf_init(&glyph_buffer,
                         glyph.box_w,
                         glyph.box_h,
                         LV_COLOR_FORMAT_A8,
                         LV_STRIDE_AUTO,
                         glyph_scratch,
                         scratch_size) != LV_RESULT_OK) {
        return false;
    }

    const lv_draw_buf_t *decoded =
        lv_font_get_glyph_bitmap(&glyph, &glyph_buffer);
    if (decoded == NULL || decoded->data == NULL) {
        lv_font_glyph_release_draw_data(&glyph);
        return false;
    }
    const uint8_t *bitmap = decoded->data;

    int16_t label_x = 1 + (face_width - (int16_t)glyph.adv_w) / 2;
    int16_t glyph_x = label_x + glyph.ofs_x;
    int16_t digit_y =
        (digit->geometry.height - digit->geometry.font->line_height) / 2;
    int16_t glyph_y = digit_y +
                      (font->line_height - font->base_line) -
                      (int16_t)glyph.box_h - glyph.ofs_y;
    int16_t split_y = digit->geometry.height / 2;

    for (int16_t row = 0; row < face_height; row++) {
        int16_t full_y = bottom ? split_y + 1 + row
                                : split_y - 2 - row;
        int16_t source_y = full_y - glyph_y;
        if (source_y < 0 || source_y >= (int16_t)glyph.box_h) {
            continue;
        }

        const uint8_t *source_row =
            bitmap + (uint32_t)source_y * decoded->header.stride;
        uint8_t *target_row = mask + (uint32_t)row * face_width;
        for (int16_t column = 0; column < face_width; column++) {
            int16_t full_x = column + 1;
            int16_t source_x = full_x - glyph_x;
            if (source_x >= 0 && source_x < (int16_t)glyph.box_w) {
                target_row[column] = source_row[source_x];
            }
        }
    }

    lv_font_glyph_release_draw_data(&glyph);
    return true;
}

static bool prepare_atlas(flip_digit_physics_t *digit)
{
    flip_digit_physics_atlas_t *atlas = digit->atlas;
    flip_digit_physics_scratch_t *scratch = digit->scratch;
    if (atlas->valid &&
        atlas->font == digit->geometry.font &&
        atlas->width == digit->geometry.width &&
        atlas->height == digit->geometry.height) {
        return true;
    }

    atlas->valid = false;
    for (uint8_t index = 0; index < 10; index++) {
        char value = (char)('0' + index);
        if (!build_half_mask(digit, value, false, atlas->top[index]) ||
            !build_half_mask(digit, value, true, atlas->bottom[index])) {
            return false;
        }
    }

    atlas->font = digit->geometry.font;
    atlas->width = digit->geometry.width;
    atlas->height = digit->geometry.height;
    atlas->valid = true;
    scratch->valid = false;
    return true;
}

static bool prepare_masks(flip_digit_physics_t *digit)
{
    flip_digit_physics_scratch_t *scratch = digit->scratch;
    if (scratch->valid &&
        scratch->font == digit->geometry.font &&
        scratch->width == digit->geometry.width &&
        scratch->height == digit->geometry.height &&
        scratch->front_digit == digit->current_digit &&
        scratch->back_digit == digit->target_digit) {
        return true;
    }

    scratch->valid = false;
    if (!prepare_atlas(digit)) {
        return false;
    }
    const size_t pixel_count =
        (size_t)(digit->geometry.width - 2) *
        (digit->geometry.height / 2 - 2);
    memcpy(scratch->front_mask,
           digit->atlas->top[digit->current_digit - '0'],
           pixel_count);
    memcpy(scratch->back_mask,
           digit->atlas->bottom[digit->target_digit - '0'],
           pixel_count);

    scratch->font = digit->geometry.font;
    scratch->width = digit->geometry.width;
    scratch->height = digit->geometry.height;
    scratch->front_digit = digit->current_digit;
    scratch->back_digit = digit->target_digit;
    scratch->valid = true;
    return true;
}

static lv_color16a_t gray_pixel(uint8_t gray, uint8_t alpha)
{
    return (lv_color16a_t){
        .lumi = gray,
        .alpha = alpha,
    };
}

static int32_t round_q15(int64_t value)
{
    const int64_t half = (int64_t)1 << 14;
    if (value >= 0) {
        return (int32_t)((value + half) >> 15);
    }
    return -(int32_t)((-value + half) >> 15);
}

static bool face_pixel_visible(const flip_digit_physics_t *digit,
                               int16_t source_x,
                               int16_t source_row,
                               int16_t face_width,
                               int16_t face_height)
{
    int16_t radius = LV_MAX(1, digit->geometry.radius - 1);
    if (source_row < face_height - radius) return true;
    if (source_x >= radius && source_x < face_width - radius) return true;

    int16_t center_x = source_x < radius
                           ? radius - 1
                           : face_width - radius;
    int16_t center_y = face_height - radius;
    int16_t delta_x = source_x - center_x;
    int16_t delta_y = source_row - center_y;
    return delta_x * delta_x + delta_y * delta_y <= radius * radius;
}

typedef struct {
    const uint8_t *row0;
    const uint8_t *row1;
    uint16_t fraction_y;
} mask_row_sampler_t;

typedef struct {
    int16_t y0;
    int16_t y1;
    uint16_t fraction_y;
    bool fully_opaque;
} face_alpha_row_sampler_t;

static mask_row_sampler_t prepare_mask_row_sampler(
    const uint8_t *mask,
    int16_t width,
    int16_t height,
    int32_t y_q8)
{
    y_q8 = LV_CLAMP(0, y_q8, (height - 1) << 8);

    int16_t y0 = (int16_t)(y_q8 >> 8);
    int16_t y1 = LV_MIN(height - 1, y0 + 1);
    return (mask_row_sampler_t){
        .row0 = mask + (uint32_t)y0 * width,
        .row1 = mask + (uint32_t)y1 * width,
        .fraction_y = (uint16_t)(y_q8 & 0xff),
    };
}

static face_alpha_row_sampler_t prepare_face_alpha_row_sampler(
    const flip_digit_physics_t *digit,
    int16_t height,
    int32_t y_q8)
{
    y_q8 = LV_CLAMP(0, y_q8, (height - 1) << 8);

    int16_t y0 = (int16_t)(y_q8 >> 8);
    int16_t y1 = LV_MIN(height - 1, y0 + 1);
    int16_t radius = LV_MAX(1, digit->geometry.radius - 1);
    return (face_alpha_row_sampler_t){
        .y0 = y0,
        .y1 = y1,
        .fraction_y = (uint16_t)(y_q8 & 0xff),
        .fully_opaque = y1 < height - radius,
    };
}

static uint8_t sample_mask_row_bilinear(
    const mask_row_sampler_t *sampler,
    int16_t width,
    int32_t x_q8)
{
    x_q8 = LV_CLAMP(0, x_q8, (width - 1) << 8);

    int16_t x0 = (int16_t)(x_q8 >> 8);
    int16_t x1 = LV_MIN(width - 1, x0 + 1);
    int32_t fraction_x = x_q8 & 0xff;
    int32_t top =
        sampler->row0[x0] * (256 - fraction_x) +
        sampler->row0[x1] * fraction_x;
    int32_t bottom =
        sampler->row1[x0] * (256 - fraction_x) +
        sampler->row1[x1] * fraction_x;
    return (uint8_t)(
        (top * (256 - sampler->fraction_y) +
         bottom * sampler->fraction_y + 32768) >>
        16);
}

static uint8_t sample_face_alpha_row_bilinear(
    const flip_digit_physics_t *digit,
    const face_alpha_row_sampler_t *sampler,
    int16_t width,
    int16_t height,
    int32_t x_q8)
{
    if (sampler->fully_opaque) {
        return 255U;
    }

    x_q8 = LV_CLAMP(0, x_q8, (width - 1) << 8);
    int16_t x0 = (int16_t)(x_q8 >> 8);
    int16_t x1 = LV_MIN(width - 1, x0 + 1);
    int32_t fraction_x = x_q8 & 0xff;
    int32_t alpha00 = face_pixel_visible(
                          digit, x0, sampler->y0, width, height)
                          ? 255
                          : 0;
    int32_t alpha10 = face_pixel_visible(
                          digit, x1, sampler->y0, width, height)
                          ? 255
                          : 0;
    int32_t alpha01 = face_pixel_visible(
                          digit, x0, sampler->y1, width, height)
                          ? 255
                          : 0;
    int32_t alpha11 = face_pixel_visible(
                          digit, x1, sampler->y1, width, height)
                          ? 255
                          : 0;
    int32_t top =
        alpha00 * (256 - fraction_x) + alpha10 * fraction_x;
    int32_t bottom =
        alpha01 * (256 - fraction_x) + alpha11 * fraction_x;
    return (uint8_t)(
        (top * (256 - sampler->fraction_y) +
         bottom * sampler->fraction_y + 32768) >>
        16);
}

static int32_t source_row_q8_for_canvas_y(int16_t canvas_y,
                                          int16_t hinge_y,
                                          int32_t abs_cos,
                                          bool back_face,
                                          int16_t face_height)
{
    int32_t distance = LV_ABS(canvas_y - hinge_y);
    int32_t depth_q8 = (int32_t)(
        ((int64_t)distance * LV_TRIGO_SIN_MAX << 8) / abs_cos);
    int32_t first_depth_q8 = (back_face ? 1 : 2) << 8;
    return LV_CLAMP(0,
                    depth_q8 - first_depth_q8,
                    (face_height - 1) << 8);
}

static int32_t projected_width_q8_for_source_row(
    int32_t source_row_q8,
    int16_t face_width,
    int16_t face_height,
    int32_t sin_value,
    bool back_face)
{
    int32_t camera_distance = face_height * 6;
    int32_t first_depth_q8 = (back_face ? 1 : 2) << 8;
    int32_t depth_q8 = source_row_q8 + first_depth_q8;
    int64_t denominator =
        ((int64_t)camera_distance * LV_TRIGO_SIN_MAX << 8) -
        (int64_t)depth_q8 * sin_value;
    int32_t scale_q15 = (int32_t)(
        ((int64_t)camera_distance <<
         (LV_TRIGO_SHIFT * 2 + 8)) /
        denominator);
    return (int32_t)(((int64_t)face_width * scale_q15) >> 7);
}

static void blend_shadow_pixel(lv_color16a_t *pixel, uint8_t alpha)
{
    if (pixel->alpha < alpha) {
        *pixel = gray_pixel(0, alpha);
    }
}

static int16_t nearest_projected_row(const int16_t *projected_y,
                                     int16_t face_height,
                                     int16_t canvas_y)
{
    int16_t nearest = 0;
    int16_t nearest_distance = LV_ABS(projected_y[0] - canvas_y);

    for (int16_t row = 1; row < face_height; row++) {
        int16_t distance = LV_ABS(projected_y[row] - canvas_y);
        if (distance < nearest_distance) {
            nearest = row;
            nearest_distance = distance;
        }
    }

    return nearest;
}

static void render_edge_on(flip_digit_physics_t *digit,
                           int16_t center_x,
                           int16_t hinge_y,
                           const int16_t *projected_width,
                           int16_t face_height,
                           int16_t window_top)
{
    int16_t canvas_width =
        digit->geometry.width + FLIP_DIGIT_PHYSICS_CANVAS_MARGIN * 2;
    int16_t canvas_height = FLIP_DIGIT_PHYSICS_MAX_CANVAS_H;
    int16_t window_bottom = window_top + canvas_height - 1;
    int16_t widest = projected_width[face_height - 1];
    int16_t left = LV_MAX(0, center_x - widest / 2);
    int16_t right = LV_MIN(canvas_width - 1, left + widest - 1);

    for (int16_t y = hinge_y - 1; y <= hinge_y + 1; y++) {
        if (y < window_top || y > window_bottom) continue;
        lv_color16a_t *row = render_pixels(digit) +
                             (uint32_t)(y - window_top) * canvas_width;
        for (int16_t x = left; x <= right; x++) {
            uint8_t gray = y == hinge_y ? 7 : 16;
            row[x] = gray_pixel(gray, LV_OPA_COVER);
        }
    }
}

static void render_fixed_hinges(flip_digit_physics_t *digit,
                                int16_t card_left,
                                int16_t card_top,
                                int16_t window_top)
{
    const int16_t hinge_width = 3;
    const int16_t hinge_height = 8;
    int16_t left_x = card_left + 2;
    int16_t right_x = card_left + digit->geometry.width - 3;
    int16_t hinge_y =
        card_top + digit->geometry.height / 2 - hinge_height / 2 + 1;
    int16_t canvas_width =
        digit->geometry.width + FLIP_DIGIT_PHYSICS_CANVAS_MARGIN * 2;

    for (int16_t y = 0; y < hinge_height; y++) {
        lv_color16a_t *row = render_pixels(digit) +
                             (uint32_t)(hinge_y + y - window_top) *
                                 canvas_width;
        for (int16_t x = 0; x < hinge_width; x++) {
            row[left_x + x] = gray_pixel(8, LV_OPA_COVER);
            row[right_x + x] = gray_pixel(8, LV_OPA_COVER);
        }
    }
}

static void publish_canvas_frame(
    flip_digit_physics_t *digit,
    flip_digit_physics_profile_t *profile)
{
    const size_t canvas_bytes =
        (size_t)(digit->geometry.width +
                 FLIP_DIGIT_PHYSICS_CANVAS_MARGIN * 2) *
        FLIP_DIGIT_PHYSICS_MAX_CANVAS_H *
        sizeof(digit->canvas_storage->canvas_pixels[0]);
    int64_t phase_start_us = esp_timer_get_time();
#if FLIP_DIGIT_STAGED_RENDER
    memcpy(digit->canvas_storage->canvas_pixels,
           render_pixels(digit),
           canvas_bytes);
    __atomic_thread_fence(__ATOMIC_RELEASE);
#else
    if (esp_ptr_external_ram(digit->canvas_storage->canvas_pixels)) {
        ESP_ERROR_CHECK(esp_cache_msync(
            digit->canvas_storage->canvas_pixels,
            canvas_bytes,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                ESP_CACHE_MSYNC_FLAG_UNALIGNED));
    }
#endif
    profile->publish_copy_us =
        elapsed_us(phase_start_us, esp_timer_get_time());

    phase_start_us = esp_timer_get_time();
    lv_obj_invalidate(digit->canvas);
    profile->invalidate_us =
        elapsed_us(phase_start_us, esp_timer_get_time());
}

static void render_angle(flip_digit_physics_t *digit,
                         uint16_t angle_deg,
                         bool contact_phase,
                         flip_digit_physics_profile_t *profile)
{
    int16_t card_width = digit->geometry.width;
    int16_t card_height = digit->geometry.height;
    int16_t face_width = card_width - 2;
    int16_t face_height = card_height / 2 - 2;
    int16_t canvas_width =
        card_width + FLIP_DIGIT_PHYSICS_CANVAS_MARGIN * 2;
    int16_t canvas_height = FLIP_DIGIT_PHYSICS_MAX_CANVAS_H;
    int16_t center_x = FLIP_DIGIT_PHYSICS_CANVAS_MARGIN + card_width / 2;
    int16_t hinge_y = FLIP_DIGIT_PHYSICS_CANVAS_Y_PAD + card_height / 2;
    int16_t card_left =
        FLIP_DIGIT_PHYSICS_CANVAS_MARGIN - PHYSICS_CARD_CONTENT_OFFSET;
    int16_t card_right = card_left + card_width - 1;
    int16_t card_top =
        FLIP_DIGIT_PHYSICS_CANVAS_Y_PAD - PHYSICS_CARD_CONTENT_OFFSET;
    int16_t card_bottom = card_top + card_height - 1;
    int16_t shadow_left = card_left + 1;
    int16_t shadow_right = card_right - 1;
    int16_t shadow_top = card_top + 1;
    int16_t shadow_bottom = card_bottom - 1;
    int32_t sin_value = lv_trigo_sin((int16_t)angle_deg);
    int32_t cos_value = lv_trigo_cos((int16_t)angle_deg);
    int32_t abs_cos = LV_ABS(cos_value);
    int32_t camera_distance = face_height * 6;
    bool back_face = angle_deg >= 90;
    int16_t window_top = back_face ? hinge_y - 4 : 0;
    int16_t window_bottom = window_top + canvas_height - 1;
    int16_t projected_y[FLIP_DIGIT_PHYSICS_MAX_FACE_H];
    int16_t projected_width[FLIP_DIGIT_PHYSICS_MAX_FACE_H];
    int64_t phase_start_us = esp_timer_get_time();

    if (!digit->suppress_publish &&
        digit->canvas_window_top != window_top) {
        digit->canvas_window_top = window_top;
        lv_obj_set_y(digit->canvas,
                     -FLIP_DIGIT_PHYSICS_CANVAS_Y_PAD +
                         PHYSICS_CARD_CONTENT_OFFSET + window_top);
    }

    memset(render_pixels(digit),
           0,
           (size_t)canvas_width * canvas_height * sizeof(lv_color16a_t));

    int16_t min_y = window_bottom + 1;
    int16_t max_y = -1;
    for (int16_t row = 0; row < face_height; row++) {
        int32_t depth = row + (back_face ? 1 : 2);
        int64_t denominator =
            (int64_t)camera_distance * LV_TRIGO_SIN_MAX -
            (int64_t)depth * sin_value;
        int32_t scale_q15 = (int32_t)(
            ((int64_t)camera_distance << (LV_TRIGO_SHIFT * 2)) /
            denominator);
        int32_t y_offset = round_q15(
            -(int64_t)depth * cos_value);
        projected_y[row] = (int16_t)(hinge_y + y_offset);
        int32_t width = LV_MAX(
            2,
            round_q15((int64_t)face_width * scale_q15));
        projected_width[row] = (int16_t)((width + 1) & ~1);
        min_y = LV_MIN(min_y, projected_y[row]);
        max_y = LV_MAX(max_y, projected_y[row]);
    }
    profile->clear_geometry_us =
        elapsed_us(phase_start_us, esp_timer_get_time());

    if (max_y - min_y < 2 || abs_cos < 900) {
        phase_start_us = esp_timer_get_time();
        render_edge_on(digit, center_x, hinge_y,
                       projected_width, face_height, window_top);
        render_fixed_hinges(digit, card_left, card_top, window_top);
        profile->raster_us =
            elapsed_us(phase_start_us, esp_timer_get_time());
        if (!digit->suppress_publish) {
            publish_canvas_frame(digit, profile);
        }
        return;
    }

    min_y = LV_MAX(window_top, min_y);
    max_y = LV_MIN(window_bottom, max_y);

    phase_start_us = esp_timer_get_time();
    if (!contact_phase) {
        uint8_t shadow_alpha = (uint8_t)(
            (90 * LV_ABS(sin_value)) / LV_TRIGO_SIN_MAX);
        for (int16_t y = min_y; y <= max_y; y++) {
            int16_t source_row =
                nearest_projected_row(projected_y, face_height, y);
            int16_t shadow_y = y + 2;
            if (shadow_y < shadow_top || shadow_y > shadow_bottom) continue;

            int16_t left = center_x - projected_width[source_row] / 2;
            int16_t right = left + projected_width[source_row] - 1;
            left = LV_MAX(shadow_left, left - 2);
            right = LV_MIN(shadow_right, right + 2);
            lv_color16a_t *shadow_row = render_pixels(digit) +
                                        (uint32_t)(shadow_y - window_top) *
                                            canvas_width;
            for (int16_t x = left; x <= right; x++) {
                blend_shadow_pixel(&shadow_row[x], shadow_alpha);
            }
        }
    }
    profile->shadow_us =
        elapsed_us(phase_start_us, esp_timer_get_time());

    phase_start_us = esp_timer_get_time();
    const uint8_t *mask = back_face ? digit->scratch->back_mask
                                    : digit->scratch->front_mask;
    uint8_t light = (uint8_t)(110 + 145 * abs_cos / LV_TRIGO_SIN_MAX);
    int32_t center_x_q8 = (center_x << 8) - 128;
    int16_t base_gray = back_face ? 35 : 27;
    int16_t text_gray = back_face ? 200 : 212;

    for (int16_t y = min_y; y <= max_y; y++) {
        int32_t source_row_q8 = source_row_q8_for_canvas_y(
            y, hinge_y, abs_cos, back_face, face_height);
        int32_t width_q8 = LV_MAX(
            256,
            projected_width_q8_for_source_row(source_row_q8,
                                               face_width,
                                               face_height,
                                               sin_value,
                                               back_face));
        int32_t left_boundary_q8 = center_x_q8 - width_q8 / 2;
        int32_t right_boundary_q8 = center_x_q8 + width_q8 / 2;
        int16_t left = (int16_t)((left_boundary_q8 + 128) >> 8);
        int16_t right = (int16_t)(
            (right_boundary_q8 - 128 + 255) >> 8);
        left = LV_MAX(0, left);
        right = LV_MIN(canvas_width - 1, right);
        lv_color16a_t *canvas_row = render_pixels(digit) +
                                    (uint32_t)(y - window_top) * canvas_width;

        mask_row_sampler_t mask_sampler =
            prepare_mask_row_sampler(mask,
                                     face_width,
                                     face_height,
                                     source_row_q8);
        face_alpha_row_sampler_t face_alpha_sampler =
            prepare_face_alpha_row_sampler(
                digit, face_height, source_row_q8);

        int32_t first_cell_left_q8 = (left << 8) - 128;
        int32_t last_cell_left_q8 = (right << 8) - 128;
        int32_t first_coverage =
            LV_MIN(first_cell_left_q8 + 256, right_boundary_q8) -
            LV_MAX(first_cell_left_q8, left_boundary_q8);
        int32_t last_coverage =
            LV_MIN(last_cell_left_q8 + 256, right_boundary_q8) -
            LV_MAX(last_cell_left_q8, left_boundary_q8);
        int64_t source_numerator =
            (int64_t)((left << 8) - left_boundary_q8) *
            face_width * 256;
        const int64_t source_delta =
            (int64_t)face_width * 65536;
        int32_t source_quotient =
            (int32_t)(source_numerator / width_q8);
        int32_t source_remainder =
            (int32_t)(source_numerator % width_q8);
        const int32_t source_step_quotient =
            (int32_t)(source_delta / width_q8);
        const int32_t source_step_remainder =
            (int32_t)(source_delta % width_q8);
        bool reseed_after_first = source_numerator < 0;

        for (int16_t x = left; x <= right; x++) {
            int32_t coverage = 256;
            if (x == left) coverage = first_coverage;
            if (x == right) coverage = LV_MIN(coverage, last_coverage);
            int32_t source_x_q8 = source_quotient - 128;
            source_x_q8 = LV_CLAMP(0,
                                   source_x_q8,
                                   (face_width - 1) << 8);
            if (coverage > 0) {
                uint8_t face_alpha =
                    sample_face_alpha_row_bilinear(
                        digit,
                        &face_alpha_sampler,
                        face_width,
                        face_height,
                        source_x_q8);
                uint8_t pixel_alpha = (uint8_t)(
                    LV_MIN(255, coverage) * face_alpha / 255);
                if (pixel_alpha != 0U) {
                    uint8_t glyph_alpha =
                        sample_mask_row_bilinear(
                            &mask_sampler,
                            face_width,
                            source_x_q8);
                    int16_t gray =
                        (base_gray * (255 - glyph_alpha) +
                         text_gray * glyph_alpha) / 255;
                    gray = gray * light / 255;

                    if (!contact_phase &&
                        (source_x_q8 < 256 ||
                         source_x_q8 >
                             (face_width - 2) << 8)) {
                        gray = gray * 2 / 5;
                    }
                    canvas_row[x] =
                        gray_pixel((uint8_t)gray, pixel_alpha);
                }
            }

            if (reseed_after_first) {
                source_numerator += source_delta;
                source_quotient =
                    (int32_t)(source_numerator / width_q8);
                source_remainder =
                    (int32_t)(source_numerator % width_q8);
                reseed_after_first = false;
            } else {
                source_quotient += source_step_quotient;
                source_remainder += source_step_remainder;
                if (source_remainder >= width_q8) {
                    source_remainder -= width_q8;
                    source_quotient++;
                }
            }
        }
    }

    render_fixed_hinges(digit, card_left, card_top, window_top);
    profile->raster_us =
        elapsed_us(phase_start_us, esp_timer_get_time());
    if (!digit->suppress_publish) {
        publish_canvas_frame(digit, profile);
    }
}

static uint16_t timeline_angle(int32_t elapsed_ms)
{
    if (elapsed_ms <= PHYSICS_CANVAS_START_END_MS) {
        return 0;
    }

    if (elapsed_ms <= PHYSICS_FLIP_END_MS) {
        return (uint16_t)(
            (elapsed_ms - PHYSICS_CANVAS_START_END_MS) * 180 /
            (PHYSICS_FLIP_END_MS - PHYSICS_CANVAS_START_END_MS));
    }

    if (elapsed_ms <= PHYSICS_REBOUND_APEX_MS) {
        int32_t progress = smoothstep_progress(
            elapsed_ms - PHYSICS_FLIP_END_MS,
            PHYSICS_REBOUND_HALF_MS);
        return (uint16_t)interpolate_progress(
            180, 180 - PHYSICS_REBOUND_DEG, progress);
    }

    if (elapsed_ms <= PHYSICS_SETTLE_END_MS) {
        int32_t progress = smoothstep_progress(
            elapsed_ms - PHYSICS_REBOUND_APEX_MS,
            PHYSICS_REBOUND_HALF_MS);
        return (uint16_t)interpolate_progress(
            180 - PHYSICS_REBOUND_DEG, 180, progress);
    }

    return 180;
}

size_t flip_digit_physics_frame_atlas_required_bytes(
    uint32_t refresh_hz_x100)
{
    if (refresh_hz_x100 == 0 ||
        refresh_hz_x100 >
            FLIP_DIGIT_PHYSICS_MAX_REFRESH_HZ_X100) {
        return 0;
    }

    const size_t slot_count =
        FLIP_DIGIT_PHYSICS_FRAME_SLOTS(refresh_hz_x100);
    if (slot_count < 2 ||
        slot_count > FLIP_DIGIT_PHYSICS_MAX_FRAME_SLOTS) {
        return 0;
    }
    if (slot_count >
        SIZE_MAX / FLIP_DIGIT_PHYSICS_DIGIT_VALUES /
            FLIP_DIGIT_PHYSICS_CANVAS_BYTES) {
        return 0;
    }
    return slot_count *
           FLIP_DIGIT_PHYSICS_DIGIT_VALUES *
           FLIP_DIGIT_PHYSICS_CANVAS_BYTES;
}

static lv_color16a_t *frame_atlas_pixels(
    const flip_digit_physics_frame_atlas_t *frame_atlas,
    uint16_t slot_index,
    uint8_t digit_index)
{
    const size_t frame_index =
        (size_t)slot_index * FLIP_DIGIT_PHYSICS_DIGIT_VALUES +
        digit_index;
    return frame_atlas->pixels +
           frame_index * FLIP_DIGIT_PHYSICS_CANVAS_PIXELS;
}

bool flip_digit_physics_frame_atlas_build(
    flip_digit_physics_t *digit,
    flip_digit_physics_frame_atlas_t *frame_atlas,
    lv_color16a_t *pixels,
    size_t capacity_bytes,
    uint32_t refresh_hz_x100)
{
    if (digit == NULL || !digit->initialized ||
        frame_atlas == NULL || pixels == NULL) {
        return false;
    }

    const size_t required_bytes =
        flip_digit_physics_frame_atlas_required_bytes(
            refresh_hz_x100);
    const uint16_t slot_count = (uint16_t)
        FLIP_DIGIT_PHYSICS_FRAME_SLOTS(refresh_hz_x100);
    if (required_bytes == 0 || capacity_bytes < required_bytes ||
        !prepare_atlas(digit)) {
        return false;
    }

    *frame_atlas = (flip_digit_physics_frame_atlas_t){
        .pixels = pixels,
        .capacity_bytes = capacity_bytes,
        .used_bytes = required_bytes,
        .refresh_hz_x100 = refresh_hz_x100,
        .slot_count = slot_count,
    };

    const int64_t build_start_us = esp_timer_get_time();
    const int16_t hinge_y =
        FLIP_DIGIT_PHYSICS_CANVAS_Y_PAD +
        digit->geometry.height / 2;
    const uint16_t flip_slot_count = (uint16_t)(
        (((uint32_t)(PHYSICS_FLIP_END_MS -
                     PHYSICS_CANVAS_START_END_MS) *
          refresh_hz_x100) +
         99999U) /
        100000U);
    const uint16_t rebound_slot_count =
        slot_count - 1U - flip_slot_count;
    if (flip_slot_count < 2U || rebound_slot_count < 2U) {
        *frame_atlas = (flip_digit_physics_frame_atlas_t){0};
        return false;
    }
    frame_atlas->flip_slot_count = flip_slot_count;
    frame_atlas->rebound_slot_count = rebound_slot_count;

    uint16_t slot_index = 0;
    frame_atlas->slots[slot_index++] =
        (flip_digit_physics_frame_slot_t){
            .elapsed_ms = PHYSICS_STATIC_START_END_MS,
            .angle_deg = 0,
            .window_top = 0,
            .visible = true,
        };

    for (uint16_t flip_index = 0;
         flip_index < flip_slot_count;
         flip_index++) {
        const uint16_t elapsed_ms = (uint16_t)(
            PHYSICS_CANVAS_START_END_MS +
            ((uint32_t)flip_index *
                 (PHYSICS_FLIP_END_MS -
                  PHYSICS_CANVAS_START_END_MS) +
             (flip_slot_count - 1U) / 2U) /
                (flip_slot_count - 1U));
        const uint16_t angle_deg = timeline_angle(elapsed_ms);
        const bool use_target_digit = angle_deg >= 90U;
        frame_atlas->slots[slot_index++] =
            (flip_digit_physics_frame_slot_t){
                .elapsed_ms = elapsed_ms,
                .angle_deg = angle_deg,
                .window_top = use_target_digit ? hinge_y - 4 : 0,
                .visible = true,
                .use_target_digit = use_target_digit,
                .contact_phase =
                    elapsed_ms >= PHYSICS_FLIP_END_MS,
            };
    }

    for (uint16_t rebound_index = 1;
         rebound_index <= rebound_slot_count;
         rebound_index++) {
        const uint16_t elapsed_ms = (uint16_t)(
            PHYSICS_FLIP_END_MS +
            ((uint32_t)rebound_index *
                 (PHYSICS_SETTLE_END_MS - PHYSICS_FLIP_END_MS) +
             rebound_slot_count / 2U) /
                rebound_slot_count);
        const uint16_t angle_deg = timeline_angle(elapsed_ms);
        frame_atlas->slots[slot_index++] =
            (flip_digit_physics_frame_slot_t){
                .elapsed_ms = elapsed_ms,
                .angle_deg = angle_deg,
                .window_top = hinge_y - 4,
                .visible = true,
                .use_target_digit = true,
                .contact_phase = true,
            };
    }
    if (slot_index != slot_count) {
        *frame_atlas = (flip_digit_physics_frame_atlas_t){0};
        return false;
    }

    digit->suppress_publish = true;
    for (uint8_t digit_index = 0;
         digit_index < FLIP_DIGIT_PHYSICS_DIGIT_VALUES;
         digit_index++) {
        memcpy(digit->scratch->front_mask,
               digit->atlas->top[digit_index],
               FLIP_DIGIT_PHYSICS_MASK_PIXELS);
        memcpy(digit->scratch->back_mask,
               digit->atlas->bottom[digit_index],
               FLIP_DIGIT_PHYSICS_MASK_PIXELS);

        for (uint16_t slot_index = 0;
             slot_index < slot_count;
             slot_index++) {
            lv_color16a_t *frame = frame_atlas_pixels(
                frame_atlas, slot_index, digit_index);
            const flip_digit_physics_frame_slot_t *slot =
                &frame_atlas->slots[slot_index];
            if (!slot->visible) {
                memset(frame, 0,
                       FLIP_DIGIT_PHYSICS_CANVAS_BYTES);
                continue;
            }

            flip_digit_physics_profile_t profile = {0};
            digit->render_target_override = frame;
            render_angle(digit,
                         slot->angle_deg,
                         slot->contact_phase,
                         &profile);
        }
    }
    digit->render_target_override = NULL;
    digit->suppress_publish = false;
    digit->scratch->valid = false;

    if (esp_ptr_external_ram(frame_atlas->pixels) &&
        esp_cache_msync(
            frame_atlas->pixels,
            required_bytes,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK) {
        *frame_atlas = (flip_digit_physics_frame_atlas_t){0};
        return false;
    }

    frame_atlas->build_us =
        elapsed_us(build_start_us, esp_timer_get_time());
    frame_atlas->valid = true;
    return true;
}

static void commit_static_target(flip_digit_physics_t *digit)
{
    flip_digit_set_value(&digit->base,
                         digit->target_digit,
                         LV_ANIM_OFF);
    digit->current_digit = digit->target_digit;
    digit->angle_deg = 180;
    digit->rendered_frame_valid = false;
    lv_obj_add_flag(digit->canvas, LV_OBJ_FLAG_HIDDEN);
}

static void converge_to_target(flip_digit_physics_t *digit)
{
    commit_static_target(digit);
    digit->animating = false;
}

#if FLIP_DIGIT_FRAME_ATLAS
static uint16_t frame_slot_for_elapsed(
    const flip_digit_physics_frame_atlas_t *frame_atlas,
    int32_t elapsed_ms)
{
    const uint16_t clamped_ms = (uint16_t)LV_CLAMP(
        0, elapsed_ms, FLIP_DIGIT_PHYSICS_ANIMATION_MS);
    uint16_t lower = 0;
    uint16_t upper = frame_atlas->slot_count;
    while (lower < upper) {
        const uint16_t middle = lower + (upper - lower) / 2U;
        if (frame_atlas->slots[middle].elapsed_ms < clamped_ms) {
            lower = middle + 1U;
        } else {
            upper = middle;
        }
    }
    if (lower == 0U) {
        return 0U;
    }
    if (lower >= frame_atlas->slot_count) {
        return frame_atlas->slot_count - 1U;
    }

    const uint16_t previous = lower - 1U;
    const uint16_t previous_delta =
        clamped_ms - frame_atlas->slots[previous].elapsed_ms;
    const uint16_t next_delta =
        frame_atlas->slots[lower].elapsed_ms - clamped_ms;
    return previous_delta < next_delta ? previous : lower;
}
#endif

static bool render_frame_atlas(
    flip_digit_physics_t *digit,
    int32_t elapsed_ms,
    int64_t frame_start_us,
    flip_digit_physics_profile_t *profile)
{
#if !FLIP_DIGIT_FRAME_ATLAS
    (void)digit;
    (void)elapsed_ms;
    (void)frame_start_us;
    (void)profile;
    return false;
#else
    flip_digit_physics_frame_atlas_t *frame_atlas =
        digit->frame_atlas;
    if (frame_atlas == NULL || !frame_atlas->valid ||
        frame_atlas->slot_count < 2U) {
        return false;
    }

    const int64_t lookup_start_us = esp_timer_get_time();
    const uint16_t slot_index =
        frame_slot_for_elapsed(frame_atlas, elapsed_ms);
    const flip_digit_physics_frame_slot_t *slot =
        &frame_atlas->slots[slot_index];
    const char source_digit =
        slot->use_target_digit
            ? digit->target_digit
            : digit->current_digit;
    const uint8_t digit_index =
        (uint8_t)(source_digit - '0');
    lv_color16a_t *frame = frame_atlas_pixels(
        frame_atlas, slot_index, digit_index);
    profile->atlas_lookup_us =
        elapsed_us(lookup_start_us, esp_timer_get_time());

    if (!slot->visible) {
        return true;
    }

    if (lv_obj_has_flag(digit->canvas, LV_OBJ_FLAG_HIDDEN)) {
        set_static_split(digit,
                         digit->target_digit,
                         digit->current_digit);
        digit->base.target_digit = digit->target_digit;
        lv_obj_remove_flag(digit->canvas, LV_OBJ_FLAG_HIDDEN);
    }
    if (slot->contact_phase) {
        lv_label_set_text_static(digit->base.bottom_label,
                                 digit_text[digit->target_digit - '0']);
    }

    if (digit->rendered_frame_valid &&
        digit->rendered_from_atlas &&
        digit->rendered_frame_slot == slot_index &&
        digit->rendered_atlas_digit == source_digit) {
        return true;
    }

    digit->angle_deg = slot->angle_deg;
    digit->rendered_angle_deg = slot->angle_deg;
    digit->rendered_contact_phase = slot->contact_phase;
    digit->rendered_frame_slot = slot_index;
    digit->rendered_atlas_digit = source_digit;
    digit->rendered_from_atlas = true;
    digit->rendered_frame_valid = true;

    const int64_t bind_start_us = esp_timer_get_time();
    if (digit->canvas_window_top != slot->window_top) {
        digit->canvas_window_top = slot->window_top;
        lv_obj_set_y(digit->canvas,
                     -FLIP_DIGIT_PHYSICS_CANVAS_Y_PAD +
                         PHYSICS_CARD_CONTENT_OFFSET +
                         slot->window_top);
    }
    lv_canvas_set_buffer(
        digit->canvas,
        frame,
        digit->geometry.width +
            FLIP_DIGIT_PHYSICS_CANVAS_MARGIN * 2,
        FLIP_DIGIT_PHYSICS_MAX_CANVAS_H,
        LV_COLOR_FORMAT_AL88);
    profile->atlas_bind_us =
        elapsed_us(bind_start_us, esp_timer_get_time());
    profile->total_us =
        elapsed_us(frame_start_us, esp_timer_get_time());

    if (digit->angle_deg != 0 && digit->frame_cb != NULL) {
        digit->frame_cb(digit->frame_user_data, profile);
    }
    return true;
#endif
}

static void physics_anim_exec_cb(void *var, int32_t value)
{
    flip_digit_physics_t *digit = (flip_digit_physics_t *)var;
    flip_digit_physics_profile_t profile = {0};
    const int64_t frame_start_us = esp_timer_get_time();

    if (value < PHYSICS_STATIC_START_END_MS) {
        return;
    }

    if (value >= PHYSICS_CANVAS_END_MS) {
        if (!lv_obj_has_flag(digit->canvas, LV_OBJ_FLAG_HIDDEN)) {
            commit_static_target(digit);
        }
        return;
    }

    if (render_frame_atlas(digit, value, frame_start_us, &profile)) {
        return;
    }

    const int64_t prepare_start_us = esp_timer_get_time();
    if (!prepare_masks(digit)) {
        converge_to_target(digit);
        return;
    }
    profile.prepare_mask_us =
        elapsed_us(prepare_start_us, esp_timer_get_time());

    if (lv_obj_has_flag(digit->canvas, LV_OBJ_FLAG_HIDDEN)) {
        set_static_split(digit,
                         digit->target_digit,
                         digit->current_digit);
        digit->base.target_digit = digit->target_digit;
        lv_obj_remove_flag(digit->canvas, LV_OBJ_FLAG_HIDDEN);
    }

    bool contact_phase = value >= PHYSICS_FLIP_END_MS;
    if (contact_phase) {
        lv_label_set_text_static(digit->base.bottom_label,
                                 digit_text[digit->target_digit - '0']);
    }

    const uint16_t angle_deg = timeline_angle(value);
    if (digit->rendered_frame_valid &&
        digit->rendered_angle_deg == angle_deg &&
        digit->rendered_contact_phase == contact_phase) {
        return;
    }

    digit->angle_deg = angle_deg;
    digit->rendered_angle_deg = angle_deg;
    digit->rendered_contact_phase = contact_phase;
    digit->rendered_from_atlas = false;
    digit->rendered_frame_valid = true;
    render_angle(digit, digit->angle_deg, contact_phase, &profile);
    profile.total_us =
        elapsed_us(frame_start_us, esp_timer_get_time());
    if (digit->angle_deg != 0 && digit->frame_cb != NULL) {
        digit->frame_cb(digit->frame_user_data, &profile);
    }
}

static void physics_anim_completed_cb(lv_anim_t *animation)
{
    flip_digit_physics_t *digit =
        (flip_digit_physics_t *)animation->var;
    if (digit->initialized && digit->animating) {
        converge_to_target(digit);
    }
}

static bool start_animation(flip_digit_physics_t *digit)
{
    set_static_split(digit,
                     digit->current_digit,
                     digit->current_digit);
    digit->base.current_digit = digit->current_digit;
    digit->base.target_digit = digit->current_digit;
    digit->animating = true;
    digit->rendered_frame_valid = false;
    lv_obj_add_flag(digit->canvas, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, digit);
    lv_anim_set_values(&animation, 0, FLIP_DIGIT_PHYSICS_ANIMATION_MS);
    lv_anim_set_duration(&animation, FLIP_DIGIT_PHYSICS_ANIMATION_MS);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_set_exec_cb(&animation, physics_anim_exec_cb);
    lv_anim_set_completed_cb(&animation, physics_anim_completed_cb);
    lv_anim_start(&animation);
    return true;
}

bool flip_digit_physics_init(flip_digit_physics_t *digit,
                                flip_digit_physics_canvas_t *canvas_storage,
                                flip_digit_physics_atlas_t *atlas,
                                flip_digit_physics_frame_atlas_t *frame_atlas,
                                flip_digit_physics_scratch_t *scratch,
                                flip_digit_physics_frame_cb_t frame_cb,
                                void *frame_user_data,
                                lv_obj_t *parent,
                                char initial_digit,
                                const flip_digit_geometry_t *geometry)
{
    if (digit == NULL || canvas_storage == NULL || atlas == NULL ||
        scratch == NULL ||
        parent == NULL ||
        geometry == NULL || geometry->font == NULL ||
        geometry->width > FLIP_DIGIT_PHYSICS_MAX_CARD_W ||
        geometry->height > FLIP_DIGIT_PHYSICS_MAX_CARD_H ||
        initial_digit < '0' || initial_digit > '9') {
        return false;
    }

    *digit = (flip_digit_physics_t){0};
    digit->geometry = *geometry;
    digit->canvas_storage = canvas_storage;
    digit->atlas = atlas;
    digit->frame_atlas = frame_atlas;
    digit->scratch = scratch;
    digit->frame_cb = frame_cb;
    digit->frame_user_data = frame_user_data;
    digit->current_digit = initial_digit;
    digit->target_digit = initial_digit;
    if (!prepare_atlas(digit)) {
        return false;
    }

    digit->root = create_root(parent, geometry->width, geometry->height);
    if (digit->root == NULL ||
        !flip_digit_init(&digit->base,
                         digit->root,
                         initial_digit,
                         geometry)) {
        return false;
    }

    int16_t canvas_width =
        geometry->width + FLIP_DIGIT_PHYSICS_CANVAS_MARGIN * 2;
    int16_t canvas_height = FLIP_DIGIT_PHYSICS_MAX_CANVAS_H;
    digit->canvas = lv_canvas_create(digit->root);
    if (digit->canvas == NULL) {
        return false;
    }
    lv_canvas_set_buffer(digit->canvas,
                         canvas_storage->canvas_pixels,
                         canvas_width,
                         canvas_height,
                         LV_COLOR_FORMAT_AL88);
    lv_obj_set_pos(digit->canvas,
                   -FLIP_DIGIT_PHYSICS_CANVAS_MARGIN +
                       PHYSICS_CARD_CONTENT_OFFSET,
                   -FLIP_DIGIT_PHYSICS_CANVAS_Y_PAD +
                       PHYSICS_CARD_CONTENT_OFFSET);
    digit->canvas_window_top = 0;
    lv_obj_remove_flag(digit->canvas,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(digit->canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(digit->canvas);

    digit->initialized = true;
    return true;
}

lv_obj_t *flip_digit_physics_get_obj(const flip_digit_physics_t *digit)
{
    if (digit == NULL || !digit->initialized) {
        return NULL;
    }
    return digit->root;
}

void flip_digit_physics_set_value(flip_digit_physics_t *digit,
                                  char next_digit,
                                  lv_anim_enable_t animate)
{
    if (digit == NULL || !digit->initialized ||
        next_digit < '0' || next_digit > '9') {
        return;
    }

    if (digit->animating) {
        if (digit->target_digit == next_digit) return;
        flip_digit_physics_finish(digit);
    }

    if (digit->current_digit == next_digit) return;
    digit->target_digit = next_digit;

    if (animate == LV_ANIM_OFF) {
        converge_to_target(digit);
        return;
    }

    start_animation(digit);
}

void flip_digit_physics_finish(flip_digit_physics_t *digit)
{
    if (digit == NULL || !digit->initialized) return;
    lv_anim_delete(digit, NULL);
    converge_to_target(digit);
}

void flip_digit_physics_deinit(flip_digit_physics_t *digit)
{
    if (digit == NULL || !digit->initialized) return;
    lv_anim_delete(digit, NULL);
    flip_digit_deinit(&digit->base);
    *digit = (flip_digit_physics_t){0};
}

bool flip_digit_physics_is_animating(const flip_digit_physics_t *digit)
{
    return digit != NULL && digit->initialized && digit->animating;
}

void flip_digit_physics_set_visual(
    flip_digit_physics_t *digit,
    flip_digit_physics_visual_t visual,
    bool selected)
{
    if (digit == NULL || !digit->initialized) {
        return;
    }

    uint32_t top_color = 0xD4D4D4;
    uint32_t bottom_color = 0xC8C8C8;
    uint32_t border_color = 0x202020;
    lv_opa_t border_opa = LV_OPA_60;
    uint32_t recolor = 0xFFFFFF;
    lv_opa_t recolor_opa = LV_OPA_TRANSP;

    if (visual == FLIP_DIGIT_PHYSICS_VISUAL_EDIT) {
        top_color = 0xF0BE5A;
        bottom_color = 0xDDA747;
        border_color = 0xD99A2B;
        border_opa = selected ? LV_OPA_COVER : LV_OPA_50;
        recolor = 0xE2A93B;
        recolor_opa = LV_OPA_COVER;
    } else if (visual == FLIP_DIGIT_PHYSICS_VISUAL_ERROR) {
        top_color = 0xFF7777;
        bottom_color = 0xE65E5E;
        border_color = 0xEF4444;
        border_opa = selected ? LV_OPA_COVER : LV_OPA_70;
        recolor = 0xEF4444;
        recolor_opa = LV_OPA_COVER;
    }

    lv_obj_t *labels[] = {
        digit->base.top_label,
        digit->base.bottom_label,
        digit->base.top_flap_label,
        digit->base.bottom_flap_label,
    };
    lv_obj_set_style_text_color(labels[0], lv_color_hex(top_color), 0);
    lv_obj_set_style_text_color(labels[1], lv_color_hex(bottom_color), 0);
    lv_obj_set_style_text_color(labels[2], lv_color_hex(top_color), 0);
    lv_obj_set_style_text_color(labels[3], lv_color_hex(bottom_color), 0);
    lv_obj_set_style_border_color(digit->base.card,
                                  lv_color_hex(border_color), 0);
    lv_obj_set_style_border_opa(digit->base.card, border_opa, 0);
    lv_obj_set_style_border_width(digit->base.card, selected ? 2 : 1, 0);
    lv_obj_set_style_image_recolor(digit->canvas,
                                   lv_color_hex(recolor), 0);
    lv_obj_set_style_image_recolor_opa(digit->canvas, recolor_opa, 0);
}
