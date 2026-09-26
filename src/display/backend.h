/*
 * moses_display -- display backend
 *
 * One backend is compiled into the program, chosen at configure time
 * (CMakeLists.txt, DISPLAY_BACKEND). It brings up its panel, hands LVGL
 * a display to draw on, and says what it is. The implementations are in
 * backend/: the Automation HAT Mini's ST7735, and an SDL window for a
 * development machine.
 *
 * Ported from the inky-pi tree (src/backend.h), which drives the same
 * Automation HAT Mini LCD plus a three-colour e-paper pHAT. Only the
 * LCD exists here, so the e-paper border control is left behind; the
 * rest of the interface is kept, because it is the seam that would let
 * moses_display drive another panel without the dashboard noticing.
 */

#ifndef __DISPLAY_BACKEND_H
#define __DISPLAY_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"


/**
 * What the backend found on the header.
 *
 * Resolution is the one LVGL draws at, so after any rotation the backend
 * applies: a panel whose controller is portrait but which is used
 * landscape reports the landscape size.
 *
 * inky-pi carries two more fields here, because it serves a three-colour
 * e-paper as well: an `accent` enum for the panel's third ink, and an
 * `epaper` flag saying refresh is slow and total, which its main() uses
 * to draw one frame and exit. Both are left out. This tree has one
 * full-colour LCD; the only thing asked of its colours is
 * backend_accent_color(), and the only refresh strategy is the
 * continuous one. A field nothing reads does not stop the next panel
 * from being driven wrongly -- it just looks as though it might.
 */
struct backend_info {
    const char *name;			/**< human-readable panel name	*/
    uint32_t hor_res;			/**< width  as LVGL sees it	*/
    uint32_t ver_res;			/**< height as LVGL sees it	*/
};


/**
 * Bring up the panel and create its LVGL display.
 *
 * Calls lv_init() and installs a tick source, so it is the first LVGL
 * call the program makes. Calling it twice is an error.
 *
 * @return < 0 in case of error
 */
int backend_init(void);

/**
 * Release the panel.
 */
void backend_deinit(void);

/**
 * The display created by backend_init(), or NULL before it ran.
 */
lv_display_t *backend_display(void);

/**
 * What backend_init() found, or NULL before it ran.
 */
const struct backend_info *backend_info(void);

/**
 * The colour this panel raises an alarm in.
 *
 * Black on a panel that has nothing better, so a caller can use it
 * unconditionally and still get something visible.
 */
lv_color_t backend_accent_color(void);

/**
 * Turn the backlight on or off.
 *
 * A no-op on a panel that has none.
 */
void backend_set_backlight(bool on);

#endif
