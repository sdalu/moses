/*
 * moses_display -- SDL backend, for a development machine
 *
 * The same screen in a window, so that moses_display can be built and
 * run somewhere other than the Raspberry Pi it is meant for. It draws
 * at the panel's exact resolution and colour format and then scales the
 * window up, because 160x80 on a desktop is a postage stamp.
 *
 * This is the second implementation of src/display/backend.h, and the
 * reason that interface exists. The program above it does not know
 * which one it was built with: the same main loop, the same MQTT
 * subscriber, the same upsd poller, the same dashboard.
 *
 * What it is for, and what it is not. It exercises everything the
 * screenshot tests cannot -- the main loop, the two source threads,
 * reconnection, what the panel does over hours rather than in one
 * rendered frame -- against a live broker. It cannot say anything about
 * the ST7735: the byte order, the RAM offset and the rotation all live
 * in the other backend. A screen that is perfect here can still be
 * wrong on the glass.
 */

#include <stdlib.h>

#include "lvgl.h"

#include "backend.h"
#include "internal.h"


/* The panel this stands in for (README, *LCD*), after rotation -- the
 * numbers the dashboard is designed against. */
#define SDL_H_RES	160
#define SDL_V_RES	80

/* Blown up, or it is a postage stamp on a desktop. Whole-number scaling
 * so each panel pixel stays a crisp square block and what is on screen
 * is honestly what the panel would show, rather than something a
 * filter has smoothed over. */
#define SDL_ZOOM	4.0f

#define SDL_TITLE	"moses_display -- 160x80"


static struct {
    lv_display_t       *disp;
    struct backend_info info;
    bool                initialized;
} sdl = {
    .info = {
	.name    = "SDL window",
	.hor_res = SDL_H_RES,
	.ver_res = SDL_V_RES,
    },
};


int
backend_init(void)
{
    if (sdl.initialized) {
	LV_LOG_ERROR("backend already initialized");
	return -1;
    }

    backend_lvgl_boot();

    lv_display_t *disp = lv_sdl_window_create(SDL_H_RES, SDL_V_RES);
    if (disp == NULL) {
	LV_LOG_ERROR("failed to create the SDL window");
	return -1;
    }

    /* The panel's format, so the window shows what the panel would --
     * including anything the dashboard's colours do at this depth. */
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB888);

    lv_sdl_window_set_zoom(disp, SDL_ZOOM);
    lv_sdl_window_set_title(disp, SDL_TITLE);

    sdl.disp        = disp;
    sdl.initialized = true;
    return 0;
}


void
backend_deinit(void)
{
    if (! sdl.initialized)
	return;

    lv_display_delete(sdl.disp);
    sdl.disp = NULL;

    lv_sdl_quit();
    sdl.initialized = false;
}


lv_display_t *
backend_display(void)
{
    return sdl.disp;
}


const struct backend_info *
backend_info(void)
{
    return sdl.initialized ? &sdl.info : NULL;
}


lv_color_t
backend_accent_color(void)
{
    /* Whatever the LCD would answer, so the two builds agree. The
     * dashboard has its own palette and does not use this. */
    return lv_color_hex(0xff0000);
}


void
backend_set_backlight(bool on)
{
    /* A window has no backlight. Not simply ignored: closing it when
     * the program asks for the light off would make Ctrl-C look like a
     * crash, and leaving it open is the honest analogue of an unlit
     * panel that is still there. */
    LV_UNUSED(on);
}
