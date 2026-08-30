idf_component_register(SRCS "perf_probe.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl
                       PRIV_REQUIRES esp_timer)
