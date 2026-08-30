/**
 * @file flip_digit_physics.h
 * @brief 透视校正的机械翻页数字（自 piconetmon time-center W4 组件移植更名）。
 */

#ifndef FLIP_DIGIT_PHYSICS_H
#define FLIP_DIGIT_PHYSICS_H

#include <stddef.h>

#include "flip_digit.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FLIP_DIGIT_STAGED_RENDER
#define FLIP_DIGIT_STAGED_RENDER 0
#endif

#ifndef FLIP_DIGIT_FRAME_ATLAS
#define FLIP_DIGIT_FRAME_ATLAS 0
#endif

/* 产品调优（相对 time-center 的 520/390）：260 ms 翻片更接近真实翻页钟的干脆感，
 * 同时让级联翻页（间隔 280 ms）的并发数恒为 1，守住 60 FPS——两卡同时翻实测 48 FPS、
 * 三卡 30 FPS，单卡逐帧光栅化 p95 约 8.7 ms 是并发的硬约束。 */
#define FLIP_DIGIT_PHYSICS_ANIMATION_MS   260
#define FLIP_DIGIT_PHYSICS_CONTACT_MS     195
#define FLIP_DIGIT_PHYSICS_MAX_CARD_W     152
#define FLIP_DIGIT_PHYSICS_MAX_CARD_H     200
#define FLIP_DIGIT_PHYSICS_CANVAS_MARGIN  8
#define FLIP_DIGIT_PHYSICS_CANVAS_Y_PAD   2
#define FLIP_DIGIT_PHYSICS_MAX_FACE_W     (FLIP_DIGIT_PHYSICS_MAX_CARD_W - 2)
#define FLIP_DIGIT_PHYSICS_MAX_FACE_H     (FLIP_DIGIT_PHYSICS_MAX_CARD_H / 2 - 2)
#define FLIP_DIGIT_PHYSICS_MASK_PIXELS     \
    (FLIP_DIGIT_PHYSICS_MAX_FACE_W * \
     FLIP_DIGIT_PHYSICS_MAX_FACE_H)
#define FLIP_DIGIT_PHYSICS_MAX_CANVAS_W   \
    (FLIP_DIGIT_PHYSICS_MAX_CARD_W + FLIP_DIGIT_PHYSICS_CANVAS_MARGIN * 2)
#define FLIP_DIGIT_PHYSICS_MAX_CANVAS_H   \
    (FLIP_DIGIT_PHYSICS_MAX_FACE_H + 8)
#define FLIP_DIGIT_PHYSICS_DIGIT_VALUES   10U
#define FLIP_DIGIT_PHYSICS_MAX_REFRESH_HZ_X100 6000U
#define FLIP_DIGIT_PHYSICS_FRAME_SLOTS(refresh_hz_x100) \
    (((FLIP_DIGIT_PHYSICS_ANIMATION_MS * (refresh_hz_x100)) + \
      99999U) / 100000U)
#define FLIP_DIGIT_PHYSICS_MAX_FRAME_SLOTS \
    FLIP_DIGIT_PHYSICS_FRAME_SLOTS( \
        FLIP_DIGIT_PHYSICS_MAX_REFRESH_HZ_X100)
#define FLIP_DIGIT_PHYSICS_CANVAS_PIXELS \
    (FLIP_DIGIT_PHYSICS_MAX_CANVAS_W * \
     FLIP_DIGIT_PHYSICS_MAX_CANVAS_H)
#define FLIP_DIGIT_PHYSICS_CANVAS_BYTES \
    (FLIP_DIGIT_PHYSICS_CANVAS_PIXELS * sizeof(lv_color16a_t))
#define FLIP_DIGIT_PHYSICS_FRAME_ATLAS_BYTES(refresh_hz_x100) \
    ((size_t)FLIP_DIGIT_PHYSICS_FRAME_SLOTS(refresh_hz_x100) * \
     FLIP_DIGIT_PHYSICS_DIGIT_VALUES * \
     FLIP_DIGIT_PHYSICS_CANVAS_BYTES)

typedef struct {
    lv_color16a_t canvas_pixels[FLIP_DIGIT_PHYSICS_CANVAS_PIXELS];
} flip_digit_physics_canvas_t;

typedef struct {
    uint8_t top[10][FLIP_DIGIT_PHYSICS_MASK_PIXELS];
    uint8_t bottom[10][FLIP_DIGIT_PHYSICS_MASK_PIXELS];
    const lv_font_t *font;
    int16_t width;
    int16_t height;
    bool valid;
} flip_digit_physics_atlas_t;

typedef struct {
    uint8_t front_mask[
        FLIP_DIGIT_PHYSICS_MASK_PIXELS];
    uint8_t back_mask[
        FLIP_DIGIT_PHYSICS_MASK_PIXELS];
#if FLIP_DIGIT_STAGED_RENDER
    lv_color16a_t render_pixels[
        FLIP_DIGIT_PHYSICS_MAX_CANVAS_W *
        FLIP_DIGIT_PHYSICS_MAX_CANVAS_H];
#endif
    const lv_font_t *font;
    int16_t width;
    int16_t height;
    char front_digit;
    char back_digit;
    bool valid;
} flip_digit_physics_scratch_t;

typedef struct {
    uint16_t elapsed_ms;
    uint16_t angle_deg;
    int16_t window_top;
    bool visible;
    bool use_target_digit;
    bool contact_phase;
} flip_digit_physics_frame_slot_t;

typedef struct {
    lv_color16a_t *pixels;
    size_t capacity_bytes;
    size_t used_bytes;
    uint32_t refresh_hz_x100;
    uint32_t build_us;
    uint16_t slot_count;
    uint16_t flip_slot_count;
    uint16_t rebound_slot_count;
    flip_digit_physics_frame_slot_t
        slots[FLIP_DIGIT_PHYSICS_MAX_FRAME_SLOTS];
    bool valid;
} flip_digit_physics_frame_atlas_t;

typedef struct {
    uint32_t prepare_mask_us;
    uint32_t clear_geometry_us;
    uint32_t shadow_us;
    uint32_t raster_us;
    uint32_t publish_copy_us;
    uint32_t invalidate_us;
    uint32_t atlas_lookup_us;
    uint32_t atlas_bind_us;
    uint32_t total_us;
} flip_digit_physics_profile_t;

typedef void (*flip_digit_physics_frame_cb_t)(
    void *user_data,
    const flip_digit_physics_profile_t *profile);

typedef struct {
    flip_digit_t base;
    flip_digit_geometry_t geometry;
    flip_digit_physics_canvas_t *canvas_storage;
    flip_digit_physics_atlas_t *atlas;
    flip_digit_physics_frame_atlas_t *frame_atlas;
    flip_digit_physics_scratch_t *scratch;
    lv_obj_t *root;
    lv_obj_t *canvas;
    lv_color16a_t *render_target_override;
    char current_digit;
    char target_digit;
    char rendered_atlas_digit;
    uint16_t angle_deg;
    uint16_t rendered_angle_deg;
    uint16_t rendered_frame_slot;
    int16_t canvas_window_top;
    flip_digit_physics_frame_cb_t frame_cb;
    void *frame_user_data;
    bool rendered_contact_phase;
    bool rendered_from_atlas;
    bool rendered_frame_valid;
    bool suppress_publish;
    bool animating;
    bool initialized;
} flip_digit_physics_t;

typedef enum {
    FLIP_DIGIT_PHYSICS_VISUAL_NORMAL = 0,
    FLIP_DIGIT_PHYSICS_VISUAL_EDIT,
    FLIP_DIGIT_PHYSICS_VISUAL_ERROR,
} flip_digit_physics_visual_t;

bool flip_digit_physics_init(flip_digit_physics_t *digit,
                                flip_digit_physics_canvas_t *canvas_storage,
                                flip_digit_physics_atlas_t *atlas,
                                flip_digit_physics_frame_atlas_t *frame_atlas,
                                flip_digit_physics_scratch_t *scratch,
                                flip_digit_physics_frame_cb_t frame_cb,
                                void *frame_user_data,
                                lv_obj_t *parent,
                                char initial_digit,
                                const flip_digit_geometry_t *geometry);

size_t flip_digit_physics_frame_atlas_required_bytes(
    uint32_t refresh_hz_x100);

bool flip_digit_physics_frame_atlas_build(
    flip_digit_physics_t *digit,
    flip_digit_physics_frame_atlas_t *frame_atlas,
    lv_color16a_t *pixels,
    size_t capacity_bytes,
    uint32_t refresh_hz_x100);

lv_obj_t *flip_digit_physics_get_obj(const flip_digit_physics_t *digit);

void flip_digit_physics_set_value(flip_digit_physics_t *digit,
                                     char next_digit,
                                     lv_anim_enable_t animate);

void flip_digit_physics_finish(flip_digit_physics_t *digit);
void flip_digit_physics_deinit(flip_digit_physics_t *digit);
bool flip_digit_physics_is_animating(const flip_digit_physics_t *digit);

/** Apply a palette and selection border without changing digit geometry. */
void flip_digit_physics_set_visual(
    flip_digit_physics_t *digit,
    flip_digit_physics_visual_t visual,
    bool selected);

#ifdef __cplusplus
}
#endif

#endif /* FLIP_DIGIT_PHYSICS_H */
