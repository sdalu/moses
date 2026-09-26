/*
 * moses_display -- the screen
 *
 * Built once, then updated from a model snapshot as often as the
 * caller likes. Nothing else in the program knows a widget exists, and
 * this file knows nothing about where a figure came from.
 */

#ifndef __DISPLAY_DASHBOARD_H
#define __DISPLAY_DASHBOARD_H

#include <time.h>

#include "lvgl.h"

#include "model.h"


/**
 * Build the screen on the display's active screen.
 *
 * Draws placeholders: every figure reads as unknown until the first
 * dashboard_update(). Call once, after a display exists, from the
 * thread that runs LVGL.
 *
 * @param accent      colour an out-of-range figure is drawn in; the
 *                    panel's, from backend_accent_color()
 * @param stale_after how many seconds a reading stays believable
 *                    without being repeated; past it, it is shown as no
 *                    longer current
 * @param device      name of the installation being watched, shown in
 *                    the header -- not the name of the machine running
 *                    this; see device_name() in src/display.c
 *
 * Everything it draws with is an argument, and nothing is read from the
 * environment: no backend, no device name, no clock. That is what lets
 * test/test_dashboard.c render this to a PNG that comes out identical
 * on every machine, and it is why this file can be linked without one
 * line of the panel code.
 */
void dashboard_create(lv_color_t accent, unsigned long stale_after,
		      const char *device);

/**
 * Put a model snapshot on the screen, as of `now`.
 *
 * `now` is the wall clock the header shows and the time readings are
 * aged against -- passed in for the same reason as the rest.
 *
 * Writes only what actually changed -- nothing at all on a tick where
 * no figure moved -- so it is cheap to call on a short interval. Must
 * run on the same thread as dashboard_create().
 */
void dashboard_update(const struct model *m, time_t now);

#endif
