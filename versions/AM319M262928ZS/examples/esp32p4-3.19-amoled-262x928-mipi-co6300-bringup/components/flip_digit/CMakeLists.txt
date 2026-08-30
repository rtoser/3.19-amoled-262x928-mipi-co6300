idf_component_register(SRCS "flip_digit.c"
                            "flip_digit_physics.c"
                            "fonts/lv_font_montserrat_bold_digits_192.c"
                            "fonts/lv_font_montserrat_bold_digits_150.c"
                       INCLUDE_DIRS "include" "fonts"
                       REQUIRES lvgl
                       PRIV_REQUIRES esp_timer esp_mm)
