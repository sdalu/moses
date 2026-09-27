/*
 * Screenshot tests for the moses_display screen.
 *
 * Renders src/display/dashboard.c onto LVGL's own headless test display
 * at the panel's size and colour format, and compares each frame with a
 * reference PNG under test/ref-imgs/. A missing reference is a failure,
 * not something to be filled in from whatever was just rendered: see
 * write_reference() below for why, and for how to add a case. A
 * mismatch writes `<name>_err.png` beside the reference, so a
 * regression can be looked at rather than guessed at.
 *
 * Run with -w it writes the references instead of comparing them.
 *
 * This exists because the screen is otherwise unknowable without
 * standing in front of the machine. It needs no panel, no GPIO, no SPI
 * and no broker -- nothing Linux-only -- so unlike the rest of
 * moses_display it runs on a development machine as well as on the Pi.
 *
 * What it proves: the layout, the fonts, the colour rules and the whole
 * model-to-pixels path, at the exact 160x80 RGB888 the panel is sent.
 *
 * What it cannot prove: that those pixels reach the glass correctly.
 * The byte order the controller reads them in, the (1,26) offset into
 * the ST7735's RAM and the 270-degree rotation all live in the
 * backend's flush path, past the point this sees.
 *
 * That gap is not hypothetical. These images were correct while the
 * panel was showing the water drop gold instead of cyan -- red and blue
 * exchanged by MADCTL's BGR bit, which src/display/backend/ now leaves
 * clear. A screenshot that matches proves the screen was drawn right,
 * and says nothing about how it was sent.
 *
 * Every input is fixed: the clock and the device name are arguments to
 * the dashboard rather than things it reads, and TZ is pinned below. So the
 * same bytes come out on every machine, which is what makes comparing
 * against a committed PNG meaningful at all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "dashboard.h"
#include "model.h"


/* Where the reference PNGs live, as CMakeLists.txt passes it -- with
 * LVGL's drive letter on the front, because LVGL's lodepng reads and
 * writes through lv_fs_* rather than stdio. Defaulted the way
 * lv_test_screenshot_compare.c defaults it, so this still compiles if
 * the definition is ever missing. */
#ifndef REF_IMGS_PATH
#define REF_IMGS_PATH	""
#endif

/* LVGL's bundled lodepng. Declared rather than included: its header is
 * not on this target's include path, and one function is all that is
 * wanted here. */
unsigned lodepng_encode32_file(const char *filename,
			       const unsigned char *image,
			       unsigned w, unsigned h);


/* The panel: 160x80 landscape (docs/hardware.md, *LCD*). */
#define PANEL_W		160
#define PANEL_H		80

/* A fixed instant for every case, so the header clock is a constant:
 * 2026-09-26 13:40:00 UTC. Only the time of day is drawn, but the date
 * is what the ages below are measured back from. */
#define NOW		((time_t)1790430000)

/* The same staleness window moses_display defaults to. */
#define STALE_AFTER	150

/* The installation being watched -- the last segment of a topic prefix
 * of `water-breaker/moses`, which is what the panel shows. */
#define DEVICE		"moses"

/* What backend_accent_color() answers. The dashboard has its own
 * palette and ignores this, but it is still the argument it takes. */
#define ACCENT		0xff0000


static int  failures = 0;
static int  checks   = 0;

/* -w: write the references rather than compare against them. */
static bool writing  = false;


/*
 * Write what is on the screen to its reference PNG. -w, and nothing else.
 *
 * LVGL will do this itself for a reference that does not exist yet, but
 * it does it inside the compare and then returns PASSED, which makes a
 * renamed or newly added case mint its own golden image and never fail
 * -- the test would be checking the renderer against itself. So
 * LV_TEST_SCREENSHOT_CREATE_REFERENCE_IMAGE is 0 in src/lv_conf.h and
 * the writing lives here instead, where it happens only when asked for,
 * and where asking costs no LVGL rebuild.
 *
 * Adding or changing a case is then: run this with -w, look at the PNG
 * it produced and decide whether it is actually right, and run without
 * -w to confirm. `git diff` and `git status` say exactly what moved,
 * which is the real check -- a reference is only worth having if someone
 * looked at it.
 *
 * The conversion is LVGL's RGB888 to the RGBA lodepng's 32-bit encoder
 * takes: a pixel is stored blue, green, red (lv_color_t in LVGL's
 * lv_color.h), so the three bytes are reversed and an opaque alpha
 * added. It has to agree with buf_to_xrgb8888() in
 * lv_test_screenshot_compare.c, on the reading side, or a reference
 * written here would not compare equal to the very screen it came from.
 * That is worth knowing is true rather than assuming: writing the
 * references and then running the comparison against them is what
 * checks it, and the committed PNGs came out byte-identical.
 */
static bool
write_reference(const char *name)
{
    /* The compare path refreshes before it reads the buffer; so must this. */
    lv_refr_now(NULL);

    lv_draw_buf_t *buf = lv_display_get_buf_active(lv_display_get_default());
    if (buf == NULL) {
	fprintf(stderr, "%s: no active draw buffer\n", name);
	return false;
    }
    if (buf->header.cf != LV_COLOR_FORMAT_RGB888) {
	fprintf(stderr, "%s: the screen is not RGB888 (cf=%d), and the "
		"conversion below assumes it is\n", name, (int)buf->header.cf);
	return false;
    }

    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;

    uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
    if (rgba == NULL) {
	fprintf(stderr, "%s: out of memory\n", name);
	return false;
    }

    const uint8_t *in  = buf->data;
    uint8_t       *out = rgba;
    for (uint32_t y = 0 ; y < h ; y++) {
	for (uint32_t x = 0 ; x < w ; x++) {
	    out[x * 4 + 0] = in[x * 3 + 2];
	    out[x * 4 + 1] = in[x * 3 + 1];
	    out[x * 4 + 2] = in[x * 3 + 0];
	    out[x * 4 + 3] = 0xff;
	}
	in  += buf->header.stride;
	out += (size_t)w * 4;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s%s", REF_IMGS_PATH, name);

    unsigned err = lodepng_encode32_file(path, rgba, w, h);
    free(rgba);

    if (err != 0) {
	fprintf(stderr, "%s: cannot write %s (lodepng error %u)\n",
		name, path, err);
	return false;
    }

    printf("wrote %s\n", name);
    return true;
}


/*
 * Render one model and either compare it with its reference or, under -w,
 * write it.
 *
 * The screen is torn down and rebuilt each time rather than updated,
 * because these are separate pictures and not successive states of one.
 */
static void
shot(const char *name, const struct model *m)
{
    lv_obj_clean(lv_screen_active());
    dashboard_create(lv_color_hex(ACCENT), STALE_AFTER, DEVICE);
    dashboard_update(m, NOW);

    checks++;

    if (writing) {
	if (! write_reference(name))
	    failures++;
	return;
    }

    if (lv_test_screenshot_compare(name) != LV_TEST_SCREENSHOT_RESULT_PASSED) {
	failures++;
	fprintf(stderr, "FAIL %s\n", name);
    }
}


/* A model where every figure is known and was read a moment ago. */
static struct model
nominal(void)
{
    struct model m;

    memset(&m, 0, sizeof(m));

    m.valve.known        = true;
    m.valve.closed       = false;		/* water flowing, as normal */
    m.valve.at           = NOW - 5;

    m.index.known        = true;
    m.index.litres       = 213025.0;
    m.index.at           = NOW - 5;

    m.temperature.known  = true;
    m.temperature.celsius = 21.4;
    m.temperature.at     = NOW - 5;

    m.ups.state          = MODEL_UPS_KNOWN;
    m.ups.charge         = 97.6;
    m.ups.runtime        = -1;			/* on mains: no time shown */
    m.ups.at             = NOW - 5;
    snprintf(m.ups.status, sizeof(m.ups.status), "%s", "HB OL CHRG");

    for (int i = 0 ; i < MODEL_PRODUCER_COUNT ; i++)
	m.avail[i] = MODEL_AVAIL_ONLINE;

    return m;
}


int
main(int argc, char **argv)
{
    if ((argc == 2) && (strcmp(argv[1], "-w") == 0)) {
	writing = true;
    } else if (argc > 1) {
	fprintf(stderr, "usage: %s [-w]\n", argv[0]);
	fprintf(stderr, "  -w  write the reference images instead of "
		"comparing against them\n");
	return EXIT_FAILURE;
    }

    /* The header shows a local time, so the zone is part of the
     * picture. Pinned, or the reference would only match in the zone it
     * was made in. */
    setenv("TZ", "UTC", 1);
    tzset();

    lv_init();

    lv_display_t *disp = lv_test_display_create(PANEL_W, PANEL_H);
    if (disp == NULL) {
	fprintf(stderr, "cannot create the test display\n");
	return EXIT_FAILURE;
    }
    /* The panel's format: the ST7735 is put in 18-bit mode and LVGL
     * renders RGB888 into it (src/lv_conf.h, LV_COLOR_FORMAT_DEFAULT).
     * Rendering the references at the same depth means they show the
     * colours the panel is actually sent. */
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB888);

    /* Everything healthy: no red anywhere, which is the state the panel
     * should be in every day of its life. */
    {
	struct model m = nominal();
	shot("dashboard-nominal.png", &m);
    }

    /* Water moving. A pulse report that counted something, inside the
     * latch window, so the flow marker is lit. */
    {
	struct model m = nominal();
	m.pulse.known      = true;
	m.pulse.count      = 3;
	m.pulse.at         = NOW - 2;
	m.pulse.latched_at = NOW - 2;
	shot("dashboard-flow.png", &m);
    }

    /* The bad day: the valve has shut, the room is at freezing and the
     * UPS is on battery with an estimated half hour left. Three reds at
     * once, which is also the widest this screen ever has to be -- if
     * anything overflows, it overflows here. */
    {
	struct model m = nominal();
	m.valve.closed        = true;
	m.temperature.celsius = 1.5;
	m.ups.charge          = 42;
	m.ups.runtime         = 1800;
	m.ups.estimated       = true;
	snprintf(m.ups.status, sizeof(m.ups.status), "%s", "OB DISCHRG");
	shot("dashboard-alarm.png", &m);
    }

    /* Nothing heard for a long time: the daemons' last wills have been
     * published and the readings are well past --stale. Everything the
     * broker ever said is still shown, in grey, because it was true
     * once; nothing is red, because nothing here is known to be wrong. */
    {
	struct model m = nominal();
	m.valve.at       = NOW - 4000;
	m.index.at       = NOW - 4000;
	m.temperature.at = NOW - 4000;
	for (int i = 0 ; i < MODEL_PRODUCER_COUNT ; i++)
	    m.avail[i] = MODEL_AVAIL_OFFLINE;
	shot("dashboard-stale.png", &m);
    }

    /* The widest the UPS line can plausibly get: on battery, low, and
     * wanting its battery replaced, with a runtime long enough to need
     * two units. If the chip clips anything, it clips here. */
    {
	struct model m = nominal();
	m.ups.charge    = 100;
	m.ups.runtime   = 3600 + 25 * 60;
	m.ups.estimated = true;
	snprintf(m.ups.status, sizeof(m.ups.status), "%s", "OB LB RB");
	shot("dashboard-ups-widest.png", &m);
    }

    /* The UPS as MQTT alone can describe it: nut-notify names a
     * transition and carries no charge and no remaining time, so the
     * state goes in words. This is what a display not on the machine
     * the UPS is attached to sees -- it has no upsd to ask. */
    {
	struct model m = nominal();
	m.ups.charge  = -1;
	m.ups.runtime = -1;
	snprintf(m.ups.status, sizeof(m.ups.status), "%s", "OB");
	shot("dashboard-ups-event.png", &m);
    }

    /* A cold start: connected to nothing, knowing nothing. Every figure
     * is a dash, the UPS included -- it has not been heard from, which
     * is not the same as there being none. */
    {
	struct model m;
	memset(&m, 0, sizeof(m));
	m.ups.charge  = -1;
	m.ups.runtime = -1;
	shot("dashboard-empty.png", &m);
    }

    if (writing) {
	printf("%s: %d references written, %d failures\n",
	       (failures == 0) ? "OK" : "FAIL", checks - failures, failures);
	if (failures == 0)
	    printf("now look at them, and run without -w to confirm\n");
    } else {
	printf("%s: %d screenshots, %d failures\n",
	       (failures == 0) ? "PASS" : "FAIL", checks, failures);
    }
    return (failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
