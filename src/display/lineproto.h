/*
 * moses_display -- reading the line protocol off the local socket
 *
 * The other daemons already emit every reading as one line of InfluxDB
 * line protocol (src/common.c, put_data()), and WITH_DGRAM sends that
 * same line to a unix datagram socket. This turns one such line back
 * into the readings the model keeps.
 *
 * It is its own translation unit, and its own test
 * (test/test_lineproto.c), for the reason src/breaker_state.c and
 * src/display/payload.c are: it decides what number reaches the panel,
 * and that is worth being able to check without a socket, a daemon or a
 * Raspberry Pi. src/display/source-unix.c is then only the socket.
 *
 * Nothing here includes mosquitto, LVGL, or anything Linux-only.
 *
 *
 * THE WIRE
 *
 *     <measurement> <field>[,<field>...] <nanosecond timestamp>
 *
 *     watermeter index=213044.000 1790489588441408829
 *     watermeter pulse=3 1790489588441408829
 *     environment temperature=21.42,pressure=102134,humidity=45.30 1790...
 *     breaker state=1 1790489588441408829
 *     watermeter failure="read" 1790489588441449592
 *
 * The measurement names are the ones the producers pass to PUT_DATA:
 * "watermeter" (src/watermeter.c), "environment" (src/sensors.c) and
 * "breaker" (src/breaker.c, NICKNAME). The two sides have to agree on
 * them, and nothing makes them -- they are string literals in four
 * files. LINEPROTO_* below are this side's, named so that a grep finds
 * both.
 *
 * The timestamp is parsed only far enough to be skipped. It says when
 * the reading was taken, which on a local socket is a moment ago, and
 * the model stamps what it stores with the time it arrived -- so using
 * it would buy nothing and would make a producer with a wrong clock
 * able to age a reading off the screen.
 *
 * Not handled, because these producers never emit it: an escaped space
 * inside a measurement name, a space inside a quoted field value, and
 * tags (`measurement,tag=x field=y`). A line using any of them is
 * refused rather than half-read.
 */

#ifndef __DISPLAY_LINEPROTO_H
#define __DISPLAY_LINEPROTO_H

#include <stdbool.h>
#include <stddef.h>


/** The measurement names, as the producers spell them. */
#define LINEPROTO_WATERMETER	"watermeter"
#define LINEPROTO_ENVIRONMENT	"environment"
#define LINEPROTO_BREAKER	"breaker"

/** Longest line worth looking at. put_data() builds its own in 768. */
#define LINEPROTO_MAX		768

/** Most readings one line can yield. `environment` carries three
 *  fields, of which one is on the panel; this is room for every field
 *  of every line above, so nothing is dropped for want of space. */
#define LINEPROTO_READINGS_MAX	4


/**
 * What one field turned out to mean.
 *
 * Only the fields the panel shows get a kind of their own. Pressure and
 * humidity are on the wire and are not drawn, so they parse and are
 * then discarded -- reported as LINEPROTO_IGNORED rather than silently,
 * so "the line was understood and had nothing for us" is distinct from
 * "the line made no sense".
 */
enum lineproto_kind {
    LINEPROTO_IGNORED = 0,	/**< understood, nothing to show	*/
    LINEPROTO_INDEX,		/**< watermeter index=, litres		*/
    LINEPROTO_PULSE,		/**< watermeter pulse=, a count		*/
    LINEPROTO_TEMPERATURE,	/**< environment temperature=, Celsius	*/
    LINEPROTO_VALVE,		/**< breaker state=, closed or not	*/
    LINEPROTO_FAILURE,		/**< failure="...", a producer said so	*/
};


/** Longest measurement name and failure reason kept. The wire has
 *  "environment" (11) and "set-state" (9); anything longer than these is
 *  from something else and is truncated rather than refused, because
 *  these two strings are only ever logged. */
#define LINEPROTO_NAME_MAX	24


/**
 * One field of one line, parsed.
 *
 * The strings are copies rather than pointers into the line: the line is
 * a datagram in a buffer that is gone by the time a caller looks at
 * these, which is exactly the kind of lifetime bug worth not having.
 */
struct lineproto_reading {
    enum lineproto_kind kind;

    union {
	double	      litres;	/**< LINEPROTO_INDEX			*/
	unsigned long count;	/**< LINEPROTO_PULSE			*/
	double	      celsius;	/**< LINEPROTO_TEMPERATURE		*/
	bool	      closed;	/**< LINEPROTO_VALVE: water shut	*/
    };

    /** Which measurement it came from, always set. */
    char measurement[LINEPROTO_NAME_MAX];

    /** LINEPROTO_FAILURE: what the producer said went wrong. */
    char failure[LINEPROTO_NAME_MAX];
};


/**
 * Parse one line into readings.
 *
 * `data` and `len` are the datagram as it arrived: not NUL-terminated,
 * and with or without a trailing newline (the socket sink strips it,
 * the stdout sink does not, and this accepts either).
 *
 * Returns the number of readings written to `out`, which is 0 for a line
 * that is well formed and has nothing this panel shows. A line that
 * cannot be parsed at all yields 0 as well, with `*ok` set false if `ok`
 * is not NULL -- the two are worth telling apart when logging, and are
 * the same thing to a caller that only wants the figures.
 *
 * Every value goes through the same parsers the MQTT source uses
 * (payload_double(), payload_ulong(), breaker_parse_state()), so a
 * reading that arrives by socket is accepted on exactly the terms one
 * arriving by broker is -- NaN and infinity refused, a fractional pulse
 * count refused, and the `state` vocabulary the breaker itself reads.
 */
size_t lineproto_parse(const char *data, size_t len,
		       struct lineproto_reading *out, size_t max, bool *ok);

#endif
