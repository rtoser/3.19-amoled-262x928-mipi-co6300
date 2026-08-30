/**
 * @file flip_digit.h
 * @brief Stateful mechanical flip-clock digit component.
 */

#ifndef FLIP_DIGIT_H
#define FLIP_DIGIT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t width;
    int16_t height;
    int16_t radius;
    const lv_font_t *font;
} flip_digit_geometry_t;

typedef enum {
    FLIP_DIGIT_IDLE = 0,
    FLIP_DIGIT_TOP_FOLD,
    FLIP_DIGIT_BOTTOM_UNFOLD,
} flip_digit_phase_t;

typedef struct {
    flip_digit_geometry_t geometry;
    lv_obj_t *card;
    lv_obj_t *top_label;
    lv_obj_t *bottom_label;
    lv_obj_t *top_flap;
    lv_obj_t *top_flap_label;
    lv_obj_t *bottom_flap;
    lv_obj_t *bottom_flap_label;
    lv_obj_t *flap_edge;
    char current_digit;
    char target_digit;
    flip_digit_phase_t phase;
    bool initialized;
} flip_digit_t;

/** Initialize one split mechanical digit with all animation layers pre-created. */
bool flip_digit_init(flip_digit_t *digit,
                        lv_obj_t *parent,
                        char initial_digit,
                        const flip_digit_geometry_t *geometry);

/** Return the component root object for layout positioning. */
lv_obj_t *flip_digit_get_obj(const flip_digit_t *digit);

/** Set the displayed digit, optionally using the two-phase flip animation. */
void flip_digit_set_value(flip_digit_t *digit,
                             char next_digit,
                             lv_anim_enable_t animate);

/** Cancel an active animation and converge immediately to its target digit. */
void flip_digit_finish(flip_digit_t *digit);

/** Cancel callbacks and clear component state before its parent is deleted. */
void flip_digit_deinit(flip_digit_t *digit);

/** Return true while either flip phase is active. */
bool flip_digit_is_animating(const flip_digit_t *digit);

#ifdef __cplusplus
}
#endif

#endif /* FLIP_DIGIT_H */
