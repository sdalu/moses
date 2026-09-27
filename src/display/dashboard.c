/*
 * moses_display -- the screen
 *
 * What the water breaker looks like from the front, on the 160x80 LCD
 * of the Automation HAT Mini:
 *
 *   +--------------------------------------+
 *   | moses           21.4°C        13:40  |  who, how warm, when
 *   |--------------------------------------|
 *   | (o)  213025 L                  [+3]  |  the meter, and flow
 *   |                                      |
 *   | +-----------------+ +--------------+ |
 *   | | VALVE           | | UPS OB       | |
 *   | | OPEN            | | 42% ~30m     | |
 *   | +-----------------+ +--------------+ |
 *   +--------------------------------------+
 *
 * Dark, for three reasons: the panel sits in a technical room where a
 * white screen at full backlight is a lamp; black is the one thing an
 * LCD renders perfectly; and it lets a colour mean something, instead
 * of being merely the part that is not ink.
 *
 * Colour carries the state, and the rule is three-valued:
 *
 *   its own colour   a current reading -- the meter cyan, a valve that
 *                    is letting water through green, the rest plain
 *   dim grey         nothing known, or nothing heard for long enough
 *                    that what is shown may no longer be true
 *   red              known and bad: the valve shut, the room at
 *                    freezing, the UPS on battery
 *
 * Keeping dim and red apart is the whole discipline of the screen. Not
 * knowing whether the valve is open is a different thing from knowing
 * it is shut, and a panel that painted both red would cry wolf every
 * time the broker hiccuped. Red means the machine is telling you
 * something.
 *
 * The hierarchy is the machine's: the meter index is what this thing
 * exists to watch, so it is the only figure in the large font and has a
 * line to itself. The valve and the UPS are what you check next, on
 * plates of their own. The temperature matters only at the ends of its
 * range, where it turns red, so it rides in the header.
 *
 * Each chip's label carries the state and its value the figures -- the
 * UPS puts its NUT flags up there, so a long status and a long runtime
 * never have to compete for one line.
 *
 * Nothing is positioned absolutely: the rows share the height between
 * them and the chips share the width, which is what lets this survive a
 * font change.
 *
 * Built once and then updated. Each field remembers what it is already
 * showing and writes nothing when that has not changed -- which is not
 * an optimisation so much as the thing that makes a periodic update
 * affordable at all: lv_label_set_text() reallocates the string and
 * marks the label dirty on every call, whatever it is handed, and a
 * dirty label is a fresh frame pushed down the SPI bus. Left to itself
 * this screen would refresh forever to show the same six values.
 *
 * test/test_dashboard.c renders all of it to PNGs under test/ref-imgs/
 * and fails when it changes, so the picture above is checked rather
 * than merely described.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "dashboard.h"
#include "model.h"
#include "ups_estimate.h"


/* Where a figure turns red.
 *
 * Cold matters more than hot in a room full of water pipes: at freezing
 * they burst, which is the damage this whole machine exists to avoid.
 * The upper bound is there to catch a room that is on fire, not a warm
 * afternoon. */
#define TEMP_COLD	4.0	/* degrees C, at or below		*/
#define TEMP_HOT	60.0	/* degrees C, at or above		*/

/* How long the flow badge stays lit after a pulse report that counted
 * anything. Long enough to be caught by a glance at a screen nobody is
 * watching, short enough to be out before the next report. */
#define FLOW_LATCH	30	/* seconds				*/

/*
 * The palette.
 *
 * Picked for an 18-bit panel: the ST7735 keeps six bits per channel, so
 * every one of these survives the trip and the two greys stay
 * distinguishable from each other and from the background once the low
 * bits are dropped. The red is deliberately not a pure 0xFF0000 -- a
 * full primary on a dark field glares and fringes on a small LCD, where
 * a slightly softened one reads as urgent without hurting to look at.
 */
#define C_BACKGROUND	0x0E1116	/* near-black, faintly blue	*/
#define C_SURFACE	0x1B2029	/* the chips			*/
#define C_HAIRLINE	0x2A313D	/* the rule under the header	*/
#define C_TEXT		0xE6EAF0	/* a reading			*/
#define C_DIM		0x6B7280	/* a label, or a stale reading	*/
#define C_WATER		0x3FC7F4	/* the meter, and the flow badge */
#define C_GOOD		0x4ADE80	/* a valve letting water through */
#define C_ALARM		0xF87171	/* known and bad		*/

/* Gaps and shapes. */
#define ROW_GAP		4	/* between the two chips		*/
#define SCREEN_PAD	2
#define SCREEN_ROW_GAP	2
#define CHIP_RADIUS	4
#define CHIP_PAD	2
#define BADGE_RADIUS	3
#define BADGE_PAD_X	3

/* A dash where a figure would be. */
#define NOTHING		"--"

/* Longest string any field holds, the UPS line being the long one. */
#define FIELD_MAX	40


/*
 * How a figure is doing, which is also how it is coloured.
 *
 * Deliberately three states and not two: see the head of this file.
 */
enum ink {
    INK_DIM = 0,	/**< unknown, or too old to trust		*/
    INK_NORMAL,		/**< a current reading, in its own colour	*/
    INK_ALARM,		/**< current, and bad				*/
};


/*
 * One thing on the screen, what it is currently showing, and the colour
 * it wears when all is well -- which differs per field, so that the
 * meter reads as the meter and a healthy valve reads as healthy.
 */
struct field {
    lv_obj_t  *label;
    char       shown[FIELD_MAX];
    enum ink   ink;
    lv_color_t normal;
};


static struct {
    struct field  clock;
    struct field  temp;
    struct field  flow;
    struct field  index;
    struct field  valve;
    struct field  valve_label;
    struct field  ups;
    struct field  ups_label;	/**< "UPS", plus the status flags	*/

    lv_obj_t     *flow_badge;	/**< shown only while water is moving	*/
    unsigned long stale_after;
} w;


//== Ink ===============================================================

static lv_color_t
ink_color(const struct field *f, enum ink ink)
{
    switch (ink) {
    case INK_ALARM:  return lv_color_hex(C_ALARM);
    case INK_NORMAL: return f->normal;
    default:         return lv_color_hex(C_DIM);
    }
}


/*
 * Show `text` in `ink`, and do nothing at all if that is already what
 * is on the screen.
 *
 * The comparison is the point: see the head of this file. A field
 * longer than it can hold is truncated rather than dropped -- a clipped
 * figure is still a figure, and nothing here gets close.
 */
static void
set(struct field *f, enum ink ink, const char *text)
{
    bool same_text = (strncmp(f->shown, text, sizeof(f->shown)) == 0);

    if (same_text && (f->ink == ink))
	return;

    if (! same_text) {
	snprintf(f->shown, sizeof(f->shown), "%s", text);
	lv_label_set_text(f->label, f->shown);
    }
    if (f->ink != ink) {
	f->ink = ink;
	lv_obj_set_style_text_color(f->label, ink_color(f, ink), LV_PART_MAIN);
    }
}


/*
 * Whether a reading taken at `at` is still worth believing, given what
 * its producer's availability topic last said.
 *
 * Two ways to stop trusting a figure: the daemon that publishes it
 * registered its last will with the broker and is gone, or it is still
 * there and has simply not said anything in too long. The second covers
 * what the first cannot -- a daemon wedged with its MQTT connection
 * still open looks online and has nothing to report.
 */
static bool
current(bool known, time_t at, enum model_avail avail, time_t now)
{
    if (! known)
	return false;
    if (avail == MODEL_AVAIL_OFFLINE)
	return false;
    return (now - at) <= (time_t)w.stale_after;
}


//== Formatting ========================================================

/*
 * A duration, as long as it needs to be and no longer.
 *
 * "45m", "2h10", "3d4h" -- two units at most, because the third is
 * never what anyone wanted to know and this screen is 160 pixels wide.
 */
static void
format_duration(char *out, size_t len, double seconds)
{
    unsigned long t = (seconds > 0) ? (unsigned long)seconds : 0;
    unsigned long d = t / 86400;
    unsigned long h = (t % 86400) / 3600;
    unsigned long m = (t % 3600) / 60;

    if (d > 0)
	snprintf(out, len, "%lud%luh", d, h);
    else if (h > 0)
	snprintf(out, len, "%luh%02lu", h, m);
    else
	snprintf(out, len, "%lum", m);
}


/*
 * The half of ups.status worth 160 pixels.
 *
 * NUT reports a set of flags and most of them are noise on a panel this
 * size. OL is the routine state, and a white reading already says the
 * power is fine; HB is the battery merely being full; CHRG and DISCHRG
 * only repeat what OL and OB have said. What survives is what is worth
 * interrupting someone with -- OB, LB -- and anything else is passed
 * through untouched, so RB, ALARM, or a flag NUT adds later still
 * reaches the panel rather than being enumerated here and missed.
 *
 * "HB OL CHRG 98%" was once wider than its row and overlapped its own
 * label, which is what this exists to stop.
 */
static void
condense_status(char *out, size_t len, const char *status)
{
    static const char *const noise[] = { "OL", "HB", "CHRG", "DISCHRG" };

    char   copy[MODEL_STATUS_MAX];
    char  *save = NULL;
    size_t at   = 0;

    out[0] = '\0';
    snprintf(copy, sizeof(copy), "%s", status);

    for (char *tok = strtok_r(copy, " ", &save) ; tok != NULL ;
	 tok = strtok_r(NULL, " ", &save)) {
	bool drop = false;
	for (size_t i = 0 ; i < sizeof(noise) / sizeof(noise[0]) ; i++)
	    if (strcmp(tok, noise[i]) == 0)
		drop = true;
	if (drop)
	    continue;

	int n = snprintf(out + at, len - at, "%s%s", (at > 0) ? " " : "", tok);
	if ((n < 0) || ((size_t)n >= len - at))
	    break;
	at += (size_t)n;
    }
}


//== The figures =======================================================

/*
 * The meter index, in whole litres -- the meter reports thousandths and
 * the screen has no room for a digit nobody reads.
 *
 * A stale index is still shown, dimmed, rather than replaced by a dash.
 * The last reading of a water meter does not stop being true when the
 * daemon publishing it goes quiet; it only stops being current, and dim
 * is exactly that statement.
 *
 * "L", not "l": at this size a lowercase l is a vertical stroke and
 * "213025 l" reads as a seven-digit number. SI allows either.
 */
static void
show_index(const struct model *m, time_t now)
{
    char buf[FIELD_MAX];

    if (! m->index.known) {
	set(&w.index, INK_DIM, NOTHING);
	return;
    }

    snprintf(buf, sizeof(buf), "%.0f L", m->index.litres);
    set(&w.index,
	current(true, m->index.at, m->avail[MODEL_WATERMETER], now)
	    ? INK_NORMAL : INK_DIM,
	buf);
}


/*
 * The flow badge.
 *
 * Lit for FLOW_LATCH seconds after a pulse report that counted
 * anything, with the count in it. Water moving is not a fault, so it is
 * not red -- it is the one thing on this screen that is news rather
 * than a state, and the latch is what makes news visible on a display
 * nobody is watching.
 *
 * Hidden outright when there is nothing to say, rather than dimmed:
 * pulse counting is optional (moses_watermeter only counts with --pin),
 * and a permanently greyed badge on a machine that does not count
 * pulses would be furniture.
 */
static void
show_flow(const struct model *m, time_t now)
{
    char buf[FIELD_MAX];

    bool flowing = (m->pulse.latched_at != 0) &&
		   ((now - m->pulse.latched_at) <= FLOW_LATCH);

    if (! flowing) {
	lv_obj_set_hidden(w.flow_badge, true);
	return;
    }

    snprintf(buf, sizeof(buf), "+%lu", m->pulse.count);
    set(&w.flow, INK_NORMAL, buf);
    lv_obj_set_hidden(w.flow_badge, false);
}


/*
 * The valve.
 *
 * Open is green and shut is red, which is the one place on this screen
 * where the safe state gets a colour of its own: on this machine a shut
 * valve is never routine. The valve is normally open and is only ever
 * energised to cut the water, so finding it shut means something
 * decided to.
 */
static void
show_valve(const struct model *m, time_t now)
{
    if (! m->valve.known) {
	set(&w.valve, INK_DIM, NOTHING);
	return;
    }

    const char *what = m->valve.closed ? "SHUT" : "OPEN";

    if (! current(true, m->valve.at, m->avail[MODEL_BREAKER], now))
	set(&w.valve, INK_DIM, what);
    else
	set(&w.valve, m->valve.closed ? INK_ALARM : INK_NORMAL, what);
}


static void
show_temp(const struct model *m, time_t now)
{
    char buf[FIELD_MAX];

    if (! m->temperature.known) {
	set(&w.temp, INK_DIM, NOTHING);
	return;
    }

    double c = m->temperature.celsius;
    snprintf(buf, sizeof(buf), "%.1f°C", c);

    if (! current(true, m->temperature.at, m->avail[MODEL_SENSORS], now))
	set(&w.temp, INK_DIM, buf);
    else
	set(&w.temp,
	    ((c <= TEMP_COLD) || (c >= TEMP_HOT)) ? INK_ALARM : INK_NORMAL,
	    buf);
}


/*
 * The UPS: what it is doing, how full it is, and -- only while it is
 * actually running off the battery -- how long that can last.
 *
 * Not aged like the rest: this one is polled by this program rather
 * than published to it, and source-nut.c writes an outcome on every
 * poll, so there is no such thing as an old reading here.
 */
static void
show_ups(const struct model *m)
{
    char buf[FIELD_MAX];
    int  n;

    switch (m->ups.state) {
    case MODEL_UPS_UNKNOWN:
	/* Nothing heard. Over MQTT that is the ordinary state of a
	 * display that has just started: upsmon sends an event when
	 * something changes and nothing in between, so there may be a
	 * long and entirely healthy wait before the first one. A dash,
	 * not "none" -- saying there is no UPS would be a claim, and
	 * this is the absence of one. */
	set(&w.ups_label, INK_DIM, "UPS");
	set(&w.ups,       INK_DIM, NOTHING);
	return;

    case MODEL_UPS_ABSENT:
	/* Looked and found nothing. On most machines that is not a
	 * fault -- but moses is battery-backed on purpose (docs/hardware.md,
	 * *Shopping list*), so it is shown rather than hidden, dimmed
	 * rather than red. */
	set(&w.ups_label, INK_DIM, "UPS");
	set(&w.ups,       INK_DIM, "none");
	return;

    case MODEL_UPS_LOST:
	/* upsd is there and will not say, or its comms to the UPS have
	 * gone. Something is being watched that would not see a power
	 * cut coming. */
	set(&w.ups_label, INK_ALARM, "UPS");
	set(&w.ups,       INK_ALARM, "lost");
	return;

    case MODEL_UPS_KNOWN:
	break;
    }

    /* ups_on_battery(), not a strstr() for "OB": this file had its own
     * substring test, which is precisely the bug test_ups_estimate.c
     * pins down -- "PROBE" contains "OB". One definition, so the panel
     * cannot disagree with the runtime estimate about what is going on. */
    enum ink ink = ups_on_battery(m->ups.status) ? INK_ALARM : INK_NORMAL;

    /* The flags go on the label line and the figures on the value line,
     * so neither has to give way to the other. */
    char flags[MODEL_STATUS_MAX];
    condense_status(flags, sizeof(flags), m->ups.status);
    if (flags[0] != '\0') {
	snprintf(buf, sizeof(buf), "UPS %s", flags);
	set(&w.ups_label, ink, buf);
    } else {
	set(&w.ups_label, ink, "UPS");
    }

    n = 0;
    if (m->ups.charge >= 0)
	n = snprintf(buf, sizeof(buf), "%.0f%%", m->ups.charge);
    if ((n >= 0) && (m->ups.runtime >= 0) && ((size_t)n < sizeof(buf))) {
	char left[16];
	format_duration(left, sizeof(left), m->ups.runtime);
	/* '~' marks a remaining time this program worked out from the
	 * charge and the current rather than one the UPS reported. It
	 * is a division, not a measurement (see source-nut.c), and
	 * printing it unmarked would be lying by omission. */
	n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s%s",
		      (n > 0) ? " " : "", m->ups.estimated ? "~" : "", left);
    }

    /* No figures at all, which is what a nut-notify event leaves: it
     * names a transition and carries no charge and no time. The state
     * then goes in the value, in words, rather than leaving the line
     * blank under a label that has already said the interesting part. */
    if (n <= 0)
	set(&w.ups, ink, ups_on_battery(m->ups.status) ? "on batt" : "on line");
    else
	set(&w.ups, ink, buf);
}


static void
show_clock(time_t now)
{
    char      buf[8];
    struct tm tm;

    if (localtime_r(&now, &tm) == NULL)
	snprintf(buf, sizeof(buf), "%s", "--:--");
    else
	strftime(buf, sizeof(buf), "%H:%M", &tm);

    set(&w.clock, INK_NORMAL, buf);
}


//== Widgets ===========================================================

/*
 * A bare container.
 *
 * lv_obj_create() arrives wearing the theme's background, border,
 * radius and padding, none of which is wanted here. Stripped, an object
 * is only what this file puts in it.
 */
static lv_obj_t *
box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    return o;
}


static lv_obj_t *
text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    return l;
}


/* A field starts out showing the placeholder it was built with. */
static void
field_init(struct field *f, lv_obj_t *parent, const lv_font_t *font,
	   uint32_t normal, const char *initial)
{
    f->normal = lv_color_hex(normal);
    f->label  = text(parent, font, normal, initial);
    f->ink    = INK_NORMAL;
    snprintf(f->shown, sizeof(f->shown), "%s", initial);
}


/* The hairline under the header, the only rule left on the screen. */
static void
hairline(lv_obj_t *parent)
{
    lv_obj_t *r = box(parent);

    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, 1);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_HAIRLINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN);
}


/*
 * A row, as tall as what it holds, laying its children out left to
 * right: first left, last right.
 *
 * Deliberately not growing. Every row growing equally is what the first
 * version of this did, and it shared the height out in four identical
 * bands -- which gave the header twice the room it needs for one line
 * of ten-point text and left the chips too short for their two. Only
 * the meter's row grows, below, so the slack lands on the figure that
 * deserves it.
 */
static lv_obj_t *
row(lv_obj_t *parent)
{
    lv_obj_t *o = box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_BETWEEN,
			  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, ROW_GAP, LV_PART_MAIN);
    return o;
}


/*
 * A chip: a rounded plate carrying a dim label with a figure under it.
 *
 * The plate is what sets the two readings you check after the meter
 * apart from the background, without spending a border or a rule on
 * them.
 *
 * `grow` is how much of the row it claims. They are not equal: "OPEN"
 * is four characters and will never be more, while the UPS line runs to
 * "OB 42% ~30m" on the day it matters -- and an even split clipped that
 * to "OB 42% ~30".
 *
 * The label gets a field of its own rather than being fixed text,
 * because the UPS puts its status flags there. Two short lines each
 * fitting is what a single long one could not do: "OB LB RB 100% ~1h25"
 * on one line lost the runtime off the end, which is the half anyone
 * actually wants when the power has gone.
 */
static void
chip(struct field *f, struct field *label_f, lv_obj_t *parent,
     const lv_font_t *small, const lv_font_t *big, const char *label,
     uint32_t normal, uint8_t grow)
{
    lv_obj_t *c = box(parent);

    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(c, grow);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER,
			  LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(c, CHIP_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, CHIP_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(c, 1, LV_PART_MAIN);

    field_init(label_f, c, small, C_DIM, label);
    field_init(f, c, big, normal, NOTHING);
}


//== The screen ========================================================

#ifdef WITH_LOG
/*
 * Report the laid-out geometry, and say so when it does not fit.
 *
 * test/test_dashboard.c shows the layout far better than this does, by
 * rendering it to a PNG. This survives alongside it because it reports
 * the *real* display: the test renders at 160x80 by construction, while
 * on the machine that resolution comes back from the backend after a
 * 270-degree rotation, and a rotation that came out wrong is exactly
 * what this would catch and the screenshots could not.
 */
static void
report_layout(lv_obj_t *screen)
{
    lv_area_t screen_area, row_area;

    /* Flex has not run yet at the end of dashboard_create(); positions
     * are whatever they were before this asks for them. */
    lv_obj_update_layout(screen);
    lv_obj_get_coords(screen, &screen_area);

    uint32_t rows   = lv_obj_get_child_count(screen);
    int32_t  bottom = screen_area.y1;

    for (uint32_t i = 0 ; i < rows ; i++) {
	lv_obj_get_coords(lv_obj_get_child(screen, i), &row_area);
	LV_LOG_USER("Layout row %u: y %d..%d (h %d)", (unsigned)i,
		    (int)(row_area.y1 - screen_area.y1),
		    (int)(row_area.y2 - screen_area.y1),
		    (int)lv_area_get_height(&row_area));
	if (row_area.y2 > bottom)
	    bottom = row_area.y2;
    }

    int32_t used  = bottom         - screen_area.y1 + 1;
    int32_t avail = screen_area.y2 - screen_area.y1 + 1;

    if (used > avail)
	LV_LOG_USER("Layout OVERFLOWS the panel: %d px of %d, %u rows",
		    (int)used, (int)avail, (unsigned)rows);
    else
	LV_LOG_USER("Layout fits: %d px of %d, %u rows",
		    (int)used, (int)avail, (unsigned)rows);
}
#endif


void
dashboard_create(lv_color_t accent, unsigned long stale_after,
		 const char *device)
{
    const lv_font_t *small = &lv_font_montserrat_10;
    const lv_font_t *body  = &lv_font_montserrat_12;
    const lv_font_t *hero  = &lv_font_montserrat_18;

    /* The alarm colour comes from the palette above rather than from
     * the backend. backend_accent_color() answers what the hardware can
     * do -- the brightest red it has -- which is the right answer for a
     * two-ink e-paper and the wrong one on a dark full-colour field,
     * where a pure primary glares. */
    LV_UNUSED(accent);

    w.stale_after = stale_after;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(C_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(screen, body, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, SCREEN_PAD, LV_PART_MAIN);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, SCREEN_ROW_GAP, LV_PART_MAIN);

    /* Which installation, how warm, and when. The name earns its place:
     * one broker can carry several of these, and a photograph of one
     * panel is otherwise indistinguishable from a photograph of
     * another. It names what is being watched, not the machine doing
     * the watching -- see device_name() in src/display.c. */
    lv_obj_t *header = row(screen);
    text(header, small, C_DIM, (device != NULL) ? device : "?");
    field_init(&w.temp,  header, small, C_TEXT, NOTHING);
    field_init(&w.clock, header, small, C_TEXT, "--:--");

    hairline(screen);

    /* The meter: what this machine exists to watch, so it gets the
     * large font and a line of its own. */
    lv_obj_t *water = row(screen);
    lv_obj_set_flex_grow(water, 1);	/* the slack belongs here */
    lv_obj_t *left  = box(water);
    /* Both of these must be content-sized in width as well as height:
     * a bare object in a flex row otherwise takes the whole line, and
     * the badge then covers the very figure it sits beside. */
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
			  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 4, LV_PART_MAIN);
    text(left, body, C_WATER, LV_SYMBOL_TINT);
    field_init(&w.index, left, hero, C_TEXT, NOTHING);

    /* The flow badge, on the water's own colour so a glance can tell it
     * from an alarm without reading it. */
    w.flow_badge = box(water);
    lv_obj_set_size(w.flow_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(w.flow_badge, lv_color_hex(C_WATER),
			      LV_PART_MAIN);
    lv_obj_set_style_bg_opa(w.flow_badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(w.flow_badge, BADGE_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(w.flow_badge, BADGE_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(w.flow_badge, 1, LV_PART_MAIN);
    field_init(&w.flow, w.flow_badge, small, C_BACKGROUND, "");
    lv_obj_set_hidden(w.flow_badge, true);

    /* The two you check next. */
    lv_obj_t *chips = row(screen);
    chip(&w.valve, &w.valve_label, chips, small, body, "VALVE", C_GOOD, 2);
    chip(&w.ups,   &w.ups_label,   chips, small, body, "UPS",   C_TEXT, 3);

#ifdef WITH_LOG
    report_layout(screen);
#endif
}


void
dashboard_update(const struct model *m, time_t now)
{
    show_clock(now);
    show_temp(m, now);
    show_index(m, now);
    show_flow(m, now);
    show_valve(m, now);
    show_ups(m);
}
