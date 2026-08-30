/**
 * @file flip_digit.c
 * @brief Stateful mechanical flip-clock digit component.
 */

#include "flip_digit.h"

#define FLIP_TOP_DURATION_MS       140
#define FLIP_BOTTOM_DURATION_MS    180

static const char *const digit_text[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};

static lv_obj_t *create_layer(lv_obj_t *parent,
                              int16_t x,
                              int16_t y,
                              int16_t width,
                              int16_t height)
{
    lv_obj_t *layer = lv_obj_create(parent);
    if (layer == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(layer);
    lv_obj_set_pos(layer, x, y);
    lv_obj_set_size(layer, width, height);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return layer;
}

static void style_half(lv_obj_t *half, uint32_t color, int16_t radius)
{
    lv_obj_set_style_bg_color(half, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(half, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(half, radius, 0);
    lv_obj_set_style_clip_corner(half, true, 0);
}

static void set_label_digit(lv_obj_t *label, char digit)
{
    lv_label_set_text_static(label, digit_text[digit - '0']);
}

static lv_obj_t *create_digit_label(lv_obj_t *half,
                                    char digit,
                                    const flip_digit_geometry_t *geometry,
                                    int16_t half_y,
                                    uint32_t color)
{
    int16_t digit_y = (geometry->height - geometry->font->line_height) / 2;

    lv_obj_t *label = lv_label_create(half);
    if (label == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(label);
    set_label_digit(label, digit);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(label, 0, digit_y - half_y);
    lv_obj_set_size(label, geometry->width - 2, geometry->font->line_height);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, geometry->font, 0);
    return label;
}

static bool create_hinge(lv_obj_t *card,
                         int16_t x,
                         const flip_digit_geometry_t *geometry)
{
    const int16_t hinge_height = 8;
    const int16_t hinge_width = 3;
    int16_t hinge_y = geometry->height / 2 - hinge_height / 2;

    lv_obj_t *hinge = create_layer(card, x, hinge_y,
                                   hinge_width, hinge_height);
    if (hinge == NULL) {
        return false;
    }
    lv_obj_set_style_bg_color(hinge, lv_color_hex(0x080808), 0);
    lv_obj_set_style_bg_opa(hinge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hinge, 1, 0);
    return true;
}

static void reset_animation_layers(flip_digit_t *digit)
{
    int16_t split_y = digit->geometry.height / 2;
    int16_t top_height = split_y - 2;
    int16_t bottom_height = digit->geometry.height - split_y - 2;
    int16_t digit_y =
        (digit->geometry.height - digit->geometry.font->line_height) / 2;

    lv_obj_set_pos(digit->top_flap, 1, 1);
    lv_obj_set_size(digit->top_flap, digit->geometry.width - 2, top_height);
    lv_obj_set_y(digit->top_flap_label, digit_y - 1);
    lv_obj_set_style_opa(digit->top_flap, LV_OPA_COVER, 0);
    lv_obj_add_flag(digit->top_flap, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(digit->bottom_flap, 1, split_y + 1);
    lv_obj_set_size(digit->bottom_flap,
                    digit->geometry.width - 2, bottom_height);
    lv_obj_set_style_opa(digit->bottom_flap, LV_OPA_COVER, 0);
    lv_obj_add_flag(digit->bottom_flap, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(digit->flap_edge, LV_OBJ_FLAG_HIDDEN);
}

static void top_fold_exec_cb(void *var, int32_t value)
{
    flip_digit_t *digit = (flip_digit_t *)var;
    int32_t top_height = digit->geometry.height / 2 - 2;
    int32_t digit_y =
        (digit->geometry.height - digit->geometry.font->line_height) / 2;
    if (value < 1) value = 1;
    if (value > top_height) value = top_height;

    int32_t y = 1 + top_height - value;
    lv_obj_set_y(digit->top_flap, y);
    lv_obj_set_height(digit->top_flap, value);
    lv_obj_set_y(digit->top_flap_label, digit_y - y);
    lv_obj_set_style_opa(digit->top_flap,
                         140 + value * 115 / top_height, 0);

    lv_obj_set_y(digit->flap_edge, y);
    lv_obj_set_style_opa(digit->flap_edge,
                         (top_height - value) * 220 / top_height, 0);
}

static void bottom_unfold_exec_cb(void *var, int32_t value)
{
    flip_digit_t *digit = (flip_digit_t *)var;
    int32_t split_y = digit->geometry.height / 2;
    int32_t bottom_height = digit->geometry.height - split_y - 2;
    if (value < 1) value = 1;
    if (value > bottom_height) value = bottom_height;

    lv_obj_set_height(digit->bottom_flap, value);
    lv_obj_set_style_opa(digit->bottom_flap,
                         150 + value * 105 / bottom_height, 0);

    lv_obj_set_y(digit->flap_edge, split_y + value);
    lv_obj_set_style_opa(digit->flap_edge,
                         220 - value * 180 / bottom_height, 0);
}

static void bottom_unfold_completed_cb(lv_anim_t *animation)
{
    flip_digit_t *digit = (flip_digit_t *)animation->var;
    if (!digit->initialized ||
        digit->phase != FLIP_DIGIT_BOTTOM_UNFOLD) {
        return;
    }

    set_label_digit(digit->bottom_label, digit->target_digit);
    digit->current_digit = digit->target_digit;
    digit->phase = FLIP_DIGIT_IDLE;
    reset_animation_layers(digit);
}

static void start_bottom_unfold(flip_digit_t *digit)
{
    int16_t split_y = digit->geometry.height / 2;
    int16_t bottom_height = digit->geometry.height - split_y - 2;

    digit->phase = FLIP_DIGIT_BOTTOM_UNFOLD;
    set_label_digit(digit->bottom_flap_label, digit->target_digit);
    lv_obj_set_pos(digit->bottom_flap, 1, split_y + 1);
    lv_obj_set_size(digit->bottom_flap, digit->geometry.width - 2, 1);
    lv_obj_set_style_opa(digit->bottom_flap, 150, 0);
    lv_obj_remove_flag(digit->bottom_flap, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(digit->flap_edge, 3, split_y + 1);
    lv_obj_set_style_opa(digit->flap_edge, 220, 0);
    lv_obj_remove_flag(digit->flap_edge, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, digit);
    lv_anim_set_values(&animation, 1, bottom_height);
    lv_anim_set_duration(&animation, FLIP_BOTTOM_DURATION_MS);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&animation, bottom_unfold_exec_cb);
    lv_anim_set_completed_cb(&animation, bottom_unfold_completed_cb);
    lv_anim_start(&animation);
}

static void top_fold_completed_cb(lv_anim_t *animation)
{
    flip_digit_t *digit = (flip_digit_t *)animation->var;
    if (!digit->initialized || digit->phase != FLIP_DIGIT_TOP_FOLD) {
        return;
    }

    lv_obj_add_flag(digit->top_flap, LV_OBJ_FLAG_HIDDEN);
    start_bottom_unfold(digit);
}

static void start_top_fold(flip_digit_t *digit)
{
    int16_t top_height = digit->geometry.height / 2 - 2;

    digit->phase = FLIP_DIGIT_TOP_FOLD;
    set_label_digit(digit->top_label, digit->target_digit);
    set_label_digit(digit->bottom_label, digit->current_digit);
    set_label_digit(digit->top_flap_label, digit->current_digit);

    lv_obj_set_pos(digit->top_flap, 1, 1);
    lv_obj_set_size(digit->top_flap,
                    digit->geometry.width - 2, top_height);
    lv_obj_set_style_opa(digit->top_flap, LV_OPA_COVER, 0);
    lv_obj_remove_flag(digit->top_flap, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(digit->flap_edge, 3, 1);
    lv_obj_set_style_opa(digit->flap_edge, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(digit->flap_edge, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, digit);
    lv_anim_set_values(&animation, top_height, 1);
    lv_anim_set_duration(&animation, FLIP_TOP_DURATION_MS);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&animation, top_fold_exec_cb);
    lv_anim_set_completed_cb(&animation, top_fold_completed_cb);
    lv_anim_start(&animation);
}

bool flip_digit_init(flip_digit_t *digit,
                     lv_obj_t *parent,
                     char initial_digit,
                     const flip_digit_geometry_t *geometry)
{
    if (digit == NULL || parent == NULL || geometry == NULL ||
        geometry->font == NULL || geometry->width < 12 ||
        geometry->height < 24 || geometry->radius < 1 ||
        initial_digit < '0' || initial_digit > '9') {
        return false;
    }

    *digit = (flip_digit_t){0};
    digit->geometry = *geometry;
    digit->current_digit = initial_digit;
    digit->target_digit = initial_digit;

    int16_t width = geometry->width;
    int16_t height = geometry->height;
    int16_t split_y = height / 2;
    int16_t top_height = split_y - 2;
    int16_t bottom_height = height - split_y - 2;

    digit->card = create_layer(parent, 0, 0, width, height);
    if (digit->card == NULL) {
        return false;
    }
    lv_obj_set_style_bg_color(digit->card, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(digit->card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(digit->card, 1, 0);
    lv_obj_set_style_border_color(digit->card, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_opa(digit->card, LV_OPA_60, 0);
    lv_obj_set_style_radius(digit->card, geometry->radius, 0);
    lv_obj_set_style_clip_corner(digit->card, true, 0);
    lv_obj_set_style_shadow_color(digit->card, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(digit->card, 8, 0);
    lv_obj_set_style_shadow_offset_y(digit->card, 4, 0);
    lv_obj_set_style_shadow_opa(digit->card, LV_OPA_80, 0);

    lv_obj_t *top = create_layer(digit->card, 1, 1,
                                 width - 2, top_height);
    if (top == NULL) {
        return false;
    }
    style_half(top, 0x1B1B1B, geometry->radius - 1);
    digit->top_label = create_digit_label(top, initial_digit, geometry,
                                          1, 0xD4D4D4);
    if (digit->top_label == NULL) {
        return false;
    }

    lv_obj_t *bottom = create_layer(digit->card, 1, split_y + 1,
                                    width - 2, bottom_height);
    if (bottom == NULL) {
        return false;
    }
    style_half(bottom, 0x232323, geometry->radius - 1);
    digit->bottom_label = create_digit_label(bottom, initial_digit, geometry,
                                             split_y + 1, 0xC8C8C8);
    if (digit->bottom_label == NULL) {
        return false;
    }

    lv_obj_t *top_lip = create_layer(digit->card, 3, 2, width - 6, 1);
    if (top_lip == NULL) {
        return false;
    }
    lv_obj_set_style_bg_color(top_lip, lv_color_hex(0x343434), 0);
    lv_obj_set_style_bg_opa(top_lip, LV_OPA_40, 0);

    digit->top_flap = create_layer(digit->card, 1, 1,
                                   width - 2, top_height);
    if (digit->top_flap == NULL) {
        return false;
    }
    style_half(digit->top_flap, 0x1B1B1B, 0);
    digit->top_flap_label =
        create_digit_label(digit->top_flap, initial_digit, geometry,
                           1, 0xD4D4D4);
    if (digit->top_flap_label == NULL) {
        return false;
    }
    lv_obj_add_flag(digit->top_flap, LV_OBJ_FLAG_HIDDEN);

    digit->bottom_flap = create_layer(digit->card, 1, split_y + 1,
                                      width - 2, bottom_height);
    if (digit->bottom_flap == NULL) {
        return false;
    }
    style_half(digit->bottom_flap, 0x232323, 0);
    digit->bottom_flap_label =
        create_digit_label(digit->bottom_flap, initial_digit, geometry,
                           split_y + 1, 0xC8C8C8);
    if (digit->bottom_flap_label == NULL) {
        return false;
    }
    lv_obj_add_flag(digit->bottom_flap, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *seam = create_layer(digit->card, 0, split_y - 1, width, 1);
    if (seam == NULL) {
        return false;
    }
    lv_obj_set_style_bg_color(seam, lv_color_hex(0x050505), 0);
    lv_obj_set_style_bg_opa(seam, LV_OPA_COVER, 0);

    lv_obj_t *seam_shadow = create_layer(digit->card, 3, split_y,
                                         width - 6, 1);
    if (seam_shadow == NULL) {
        return false;
    }
    lv_obj_set_style_bg_color(seam_shadow, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(seam_shadow, LV_OPA_40, 0);

    if (!create_hinge(digit->card, 1, geometry) ||
        !create_hinge(digit->card, width - 4, geometry)) {
        return false;
    }

    digit->flap_edge = create_layer(digit->card, 3, 1, width - 6, 2);
    if (digit->flap_edge == NULL) {
        return false;
    }
    lv_obj_set_style_bg_color(digit->flap_edge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(digit->flap_edge, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(digit->flap_edge, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(digit->flap_edge, 3, 0);
    lv_obj_set_style_shadow_opa(digit->flap_edge, LV_OPA_60, 0);
    lv_obj_add_flag(digit->flap_edge, LV_OBJ_FLAG_HIDDEN);

    digit->initialized = true;
    return true;
}

lv_obj_t *flip_digit_get_obj(const flip_digit_t *digit)
{
    if (digit == NULL || !digit->initialized) {
        return NULL;
    }
    return digit->card;
}

void flip_digit_set_value(flip_digit_t *digit,
                          char next_digit,
                          lv_anim_enable_t animate)
{
    if (digit == NULL || !digit->initialized ||
        next_digit < '0' || next_digit > '9') {
        return;
    }

    if (digit->phase != FLIP_DIGIT_IDLE) {
        if (digit->target_digit == next_digit) {
            return;
        }
        flip_digit_finish(digit);
    }

    if (digit->current_digit == next_digit) {
        return;
    }

    digit->target_digit = next_digit;
    if (animate == LV_ANIM_OFF) {
        flip_digit_finish(digit);
        return;
    }

    start_top_fold(digit);
}

void flip_digit_finish(flip_digit_t *digit)
{
    if (digit == NULL || !digit->initialized) {
        return;
    }

    lv_anim_delete(digit, NULL);
    set_label_digit(digit->top_label, digit->target_digit);
    set_label_digit(digit->bottom_label, digit->target_digit);
    digit->current_digit = digit->target_digit;
    digit->phase = FLIP_DIGIT_IDLE;
    reset_animation_layers(digit);
}

void flip_digit_deinit(flip_digit_t *digit)
{
    if (digit == NULL || !digit->initialized) {
        return;
    }

    lv_anim_delete(digit, NULL);
    *digit = (flip_digit_t){0};
}

bool flip_digit_is_animating(const flip_digit_t *digit)
{
    return digit != NULL && digit->initialized &&
           digit->phase != FLIP_DIGIT_IDLE;
}
