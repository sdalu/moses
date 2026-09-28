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
 * Colour carries the state, and the rule has four values:
 *
 *   its own colour   a current reading -- the meter cyan, a valve that
 *                    is letting water through green, the rest plain
 *   dim grey         nothing known, or nothing heard for long enough
 *                    that what is shown may no longer be true
 *   red              known and bad: the valve shut, the room at
 *                    freezing, the UPS on battery
 *   amber            suspect: something is probably wrong and nobody
 *                    can say so yet -- the leak report's "never quiet"
 *                    warning, and nothing else so far
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
 * A leak report (docs/leak.md) takes the header's place while it is
 * not ok -- the kind and rate on the left, how long on the right:
 *
 *   +--------------------------------------+
 *   | ~ Flow 5.6 L/min             42 min  |  red: a flow or a drip
 *   |--------------------------------------|  amber: never quiet
 *   | (o)  213025 L                  [+6]  |
 *   ...
 *
 * The header is the row to give up: the meter, the flow and the valve
 * are exactly what a leak makes worth reading, and the name and the
 * clock can wait. The temperature cannot, when it is red too: it gets a
 * red box of its own at the banner's right, behind a snowflake (a flame
 * for a room far too hot), and the duration gives it the room -- the
 * banner reads as split in two, the leak in its colour and the room in
 * red. The same red box holds it in the header when there is no leak.
 * A report is retained and only published on a change, so it is not
 * aged like a reading; it goes grey when the watermeter is gone.
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


/* The leak banner's icons (src/display/icons.c): Font Awesome glyphs at
 * 10 px over Montserrat 10, so an icon and its words share one label
 * and one baseline. UTF-8 of the code points listed there. */
LV_FONT_DECLARE(moses_icons_10)
#define ICON_FLOOD	"\xEE\x94\x8E"	/* U+E50E house-flood-water	*/
#define ICON_DRIP	"\xEE\x80\x86"	/* U+E006 faucet-drip		*/
#define ICON_QUIET	"\xEE\x80\x85"	/* U+E005 faucet		*/
#define ICON_COLD	"\xEF\x8B\x9C"	/* U+F2DC snowflake		*/
#define ICON_HOT	"\xEF\x81\xAD"	/* U+F06D fire			*/


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
#define C_WARN		0xF5A524	/* suspect: a warning, not an alarm */

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
    INK_ON_ALARM,	/**< current and bad, dark on its own red box	*/
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

    lv_obj_t     *header;	/**< name, temperature, clock		*/
    lv_obj_t     *temp_box;	/**< red around the temperature, when bad */
    bool          temp_boxed;
    lv_obj_t     *banner;	/**< a leak report, in the header's place */
    lv_obj_t     *banner_box;	/**< the leak, in its colour		*/
    lv_obj_t     *banner_temp;	/**< the temperature, when bad, in red	*/
    struct field  banner_tval;
    struct field  banner_what;	/**< "! Flow 5.6 L/min"			*/
    struct field  banner_long;	/**< "42 min"				*/
    int           banner_look;	/**< enum banner_look, -1 before any	*/

    unsigned long stale_after;
} w;


/* How the banner is painted. */
enum banner_look {
    BANNER_ALERT = 0,		/**< red ground, dark text: known and bad */
    BANNER_WARN,		/**< amber ground, dark text: a suspicion */
    BANNER_STALE,		/**< the chip colour, dim text: the producer is gone */
};


//== Ink ===============================================================

static lv_color_t
ink_color(const struct field *f, enum ink ink)
{
    switch (ink) {
    case INK_ALARM:  return lv_color_hex(C_ALARM);
    case INK_ON_ALARM: return lv_color_hex(C_BACKGROUND);
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
 * How long a leak has gone on, spelt out for the banner: "42 min",
 * "5 h 12", "26 h". Longer than format_duration()'s "5h12" because the
 * banner is read from across a room, by someone who did not know it
 * was there, and has the width for it.
 */
static void
format_leak_duration(char *out, size_t len, time_t seconds)
{
    unsigned long t = (seconds > 0) ? (unsigned long)seconds : 0;
    unsigned long h = t / 3600;
    unsigned long m = (t % 3600) / 60;

    if (h >= 24)
	snprintf(out, len, "%lu h", h);
    else if (h > 0)
	snprintf(out, len, "%lu h %02lu", h, m);
    else
	snprintf(out, len, "%lu min", m);
}


/*
 * The half of ups.status worth 160 pixels.
 *
 * NUT reports a set of flags and most of them are noise on a panel this
 * size. OL and OB are said in words instead, "mains" or "battery" (see
 * show_ups()); HB is the battery merely being full; CHRG and DISCHRG
 * only repeat what OL and OB have said. What survives is what is worth
 * interrupting someone with -- LB -- and anything else is passed
 * through untouched, so RB, ALARM, or a flag NUT adds later still
 * reaches the panel rather than being enumerated here and missed.
 *
 * "HB OL CHRG 98%" was once wider than its row and overlapped its own
 * label, which is what this exists to stop.
 */
/* Whether `flag` is one of the space-separated flags of `status`. */
static bool
has_flag(const char *status, const char *flag)
{
    char  copy[MODEL_STATUS_MAX];
    char *save = NULL;

    snprintf(copy, sizeof(copy), "%s", status);
    for (char *tok = strtok_r(copy, " ", &save) ; tok != NULL ;
	 tok = strtok_r(NULL, " ", &save))
	if (strcmp(tok, flag) == 0)
	    return true;
    return false;
}


static void
condense_status(char *out, size_t len, const char *status)
{
    static const char *const noise[] = { "OL", "OB", "HB", "CHRG", "DISCHRG" };

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


/*
 * A red box around a figure, or none.
 *
 * The temperature is the one figure that can be red in the header and
 * beside a leak banner, and the banner has colours of its own -- amber
 * for a suspicion, grey for a report nobody vouches for any more. A red
 * word on amber or grey reads as neither, so an alarming temperature
 * gets its own red box, with its icon, wherever it is: the header, or
 * the banner's right-hand end, which then looks split in two.
 */
static void
red_box(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(b, on ? 3 : 0, LV_PART_MAIN);
}


/* The temperature when it is at an end of its range, with its icon:
 * a snowflake at freezing, a flame far too hot. False otherwise. */
static bool
temp_alarm(const struct model *m, time_t now, char *out, size_t len)
{
    if (! m->temperature.known ||
	! current(true, m->temperature.at, m->avail[MODEL_SENSORS], now))
	return false;

    double c = m->temperature.celsius;
    if ((c > TEMP_COLD) && (c < TEMP_HOT))
	return false;

    snprintf(out, len, "%s %.1f°C", (c <= TEMP_COLD) ? ICON_COLD : ICON_HOT, c);
    return true;
}


static void
show_temp(const struct model *m, time_t now)
{
    char buf[FIELD_MAX];
    bool boxed = temp_alarm(m, now, buf, sizeof(buf));

    if (boxed) {
	set(&w.temp, INK_ON_ALARM, buf);
    } else if (! m->temperature.known) {
	set(&w.temp, INK_DIM, NOTHING);
    } else {
	snprintf(buf, sizeof(buf), "%.1f°C", m->temperature.celsius);
	set(&w.temp,
	    current(true, m->temperature.at, m->avail[MODEL_SENSORS], now)
		? INK_NORMAL : INK_DIM,
	    buf);
    }

    if (boxed != w.temp_boxed) {
	w.temp_boxed = boxed;
	red_box(w.temp_box, boxed);
    }
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

    /* Where the power is coming from, in words, then the flags worth
     * seeing -- all on the label line, and the figures on the value
     * line, so neither has to give way to the other. A status that says
     * neither OL nor on-battery (an event name, a UPS that has not
     * settled) gets no word rather than a guess. */
    char flags[MODEL_STATUS_MAX];
    condense_status(flags, sizeof(flags), m->ups.status);

    /* "battery" gives way to "batt" when flags follow it: "UPS battery
     * LB RB" is wider than the chip, and the flag cut off would be the
     * one worth reading (test/ref-imgs/dashboard-ups-widest.png). */
    const char *power = (ink == INK_ALARM)
			    ? ((flags[0] != '\0') ? "batt" : "battery")
		      : has_flag(m->ups.status, "OL") ? "mains"
		      : NULL;
    snprintf(buf, sizeof(buf), "UPS%s%s%s%s",
	     power ? " " : "", power ? power : "",
	     (flags[0] != '\0') ? " " : "", flags);
    set(&w.ups_label, ink, buf);

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


/*
 * The leak banner, or the header when there is nothing to say.
 *
 * Painted only when its look changes, and its two texts only when they
 * change -- the duration moves once a minute, which is then the one
 * redraw a standing leak costs.
 */
static void
banner_paint(enum banner_look look)
{
    if (w.banner_look == (int)look)
	return;
    w.banner_look = (int)look;

    uint32_t ground = (look == BANNER_ALERT) ? C_ALARM
		    : (look == BANNER_WARN)  ? C_WARN
		    :                          C_SURFACE;
    uint32_t ink    = (look == BANNER_STALE) ? C_DIM : C_BACKGROUND;

    lv_obj_set_style_bg_color(w.banner_box, lv_color_hex(ground), LV_PART_MAIN);
    w.banner_what.normal = lv_color_hex(ink);
    w.banner_long.normal = lv_color_hex(ink);
    lv_obj_set_style_text_color(w.banner_what.label, lv_color_hex(ink), LV_PART_MAIN);
    lv_obj_set_style_text_color(w.banner_long.label, lv_color_hex(ink), LV_PART_MAIN);
}

static void
show_leak(const struct model *m, time_t now)
{
    char what[FIELD_MAX], howlong[FIELD_MAX];

    bool shown = m->leak.known && (m->leak.level != MODEL_LEAK_OK) &&
		 (m->leak.kind != MODEL_LEAK_NONE);
    lv_obj_set_hidden(w.header, shown);
    lv_obj_set_hidden(w.banner, ! shown);
    if (! shown)
	return;

    switch (m->leak.kind) {
    case MODEL_LEAK_FLOW:
	snprintf(what, sizeof(what), ICON_FLOOD " Flow %.1f L/min", m->leak.rate);
	break;
    case MODEL_LEAK_SLOW:
	snprintf(what, sizeof(what), ICON_DRIP " Drip %.1f L/h", m->leak.rate);
	break;
    default:
	snprintf(what, sizeof(what), "%s", ICON_QUIET " Never quiet");
	break;
    }

    /* A freezing or burning room is red in the header this replaces,
     * and matters as much: it gets a red box of its own at the right,
     * whatever colour the leak is, and the duration gives it the room
     * (there are 156 pixels for the two). */
    char temp[FIELD_MAX];
    bool hot_or_cold = temp_alarm(m, now, temp, sizeof(temp));
    lv_obj_set_hidden(w.banner_temp, ! hot_or_cold);
    if (hot_or_cold)
	set(&w.banner_tval, INK_ON_ALARM, temp);

    if ((! hot_or_cold) && (m->leak.since > 0) && (m->leak.since <= now))
	format_leak_duration(howlong, sizeof(howlong), now - m->leak.since);
    else
	howlong[0] = '\0';

    banner_paint((m->avail[MODEL_WATERMETER] == MODEL_AVAIL_OFFLINE) ? BANNER_STALE
		 : (m->leak.level == MODEL_LEAK_ALERT)                ? BANNER_ALERT
		 :                                                       BANNER_WARN);
    set(&w.banner_what, INK_NORMAL, what);
    set(&w.banner_long, INK_NORMAL, howlong);
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
    char      spans[128] = "";
    size_t    len        = 0;

    /* Flex has not run yet at the end of dashboard_create(); positions
     * are whatever they were before this asks for them. */
    lv_obj_update_layout(screen);
    lv_obj_get_coords(screen, &screen_area);

    uint32_t rows   = lv_obj_get_child_count(screen);
    int32_t  bottom = screen_area.y1;

    /* Each row as the lines it spans, top to bottom. */
    for (uint32_t i = 0 ; i < rows ; i++) {
	if (lv_obj_is_hidden(lv_obj_get_child(screen, i)))
	    continue;			/* the banner, until a leak */
	lv_obj_get_coords(lv_obj_get_child(screen, i), &row_area);
	if (len < sizeof(spans))
	    len += snprintf(spans + len, sizeof(spans) - len, "%s%d..%d",
			    (i > 0) ? " " : "",
			    (int)(row_area.y1 - screen_area.y1),
			    (int)(row_area.y2 - screen_area.y1));
	if (row_area.y2 > bottom)
	    bottom = row_area.y2;
    }

    int32_t used  = bottom         - screen_area.y1 + 1;
    int32_t avail = screen_area.y2 - screen_area.y1 + 1;

    /* One line, in the daemons' "Label : value" shape rather than
     * LVGL's log prefix, which names this file and line to no one's
     * benefit. */
    fprintf(stderr, "Layout               : %s, %d px of %d, rows %s\n",
	    (used > avail) ? "OVERFLOWS" : "fits",
	    (int)used, (int)avail, spans);
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
    w.header = header;
    text(header, small, C_DIM, (device != NULL) ? device : "?");
    /* The temperature sits in a box that is invisible until the room is
     * at an end of its range (red_box()). Its font is the icon font, for
     * the snowflake; everything else in it falls back to Montserrat 10,
     * so an ordinary reading is drawn exactly as the small font draws it. */
    w.temp_boxed = false;		/* a fresh box; see red_box() */
    w.temp_box = box(header);
    lv_obj_set_size(w.temp_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(w.temp_box, lv_color_hex(C_ALARM), LV_PART_MAIN);
    lv_obj_set_style_radius(w.temp_box, 2, LV_PART_MAIN);
    red_box(w.temp_box, false);
    field_init(&w.temp,  w.temp_box, &moses_icons_10, C_TEXT, NOTHING);
    field_init(&w.clock, header, small, C_TEXT, "--:--");

    /* The leak banner, hidden in the header's place until a report is
     * not ok (show_leak()). Same font and height as the header, so the
     * rows below do not move when it comes and goes. */
    w.banner = row(screen);
    lv_obj_set_style_pad_column(w.banner, 3, LV_PART_MAIN);

    w.banner_box = row(w.banner);	/* the leak: kind, rate, how long */
    lv_obj_set_width(w.banner_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(w.banner_box, 1);
    lv_obj_set_style_bg_opa(w.banner_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(w.banner_box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(w.banner_box, 3, LV_PART_MAIN);
    field_init(&w.banner_what, w.banner_box, &moses_icons_10, C_BACKGROUND, "");
    field_init(&w.banner_long, w.banner_box, &moses_icons_10, C_BACKGROUND, "");

    w.banner_temp = box(w.banner);	/* the room, when it is bad */
    lv_obj_set_size(w.banner_temp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(w.banner_temp, lv_color_hex(C_ALARM), LV_PART_MAIN);
    lv_obj_set_style_radius(w.banner_temp, 2, LV_PART_MAIN);
    red_box(w.banner_temp, true);
    field_init(&w.banner_tval, w.banner_temp, &moses_icons_10, C_BACKGROUND, "");
    lv_obj_set_hidden(w.banner_temp, true);
    w.banner_look = -1;
    banner_paint(BANNER_ALERT);
    lv_obj_set_hidden(w.banner, true);

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
    show_leak(m, now);
    show_clock(now);
    show_temp(m, now);
    show_index(m, now);
    show_flow(m, now);
    show_valve(m, now);
    show_ups(m);
}
