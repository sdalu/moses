/*
 * moses_display -- what a backend needs and does not own
 *
 * Bringing LVGL up is the same two calls whichever panel is on the
 * header, so they sit here rather than in the one backend that exists.
 */

#ifndef __DISPLAY_BACKEND_INTERNAL_H
#define __DISPLAY_BACKEND_INTERNAL_H

#include <time.h>

#include "lvgl.h"


/**
 * Milliseconds for lv_tick_set_cb().
 *
 * CLOCK_MONOTONIC rather than gettimeofday(): LVGL measures durations
 * with this, and a clock that can be stepped backwards by NTP turns a
 * timer into one that never fires.
 */
static inline uint32_t
backend_tick_get_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


/**
 * Start LVGL and give it a clock.
 */
static inline void
backend_lvgl_boot(void)
{
    lv_init();
    lv_tick_set_cb(backend_tick_get_cb);
}

#endif
