/*
 * moses_display -- what a UPS reading means
 *
 * The arithmetic and the flag-reading, kept apart from the socket that
 * fetched them (src/display/source-nut.c) so it can be tested without
 * a upsd, a UPS, or a Raspberry Pi -- the same reason src/breaker_state.c
 * is its own file. Nothing here includes LVGL, mosquitto or a header
 * that is Linux-only.
 */

#ifndef __DISPLAY_UPS_ESTIMATE_H
#define __DISPLAY_UPS_ESTIMATE_H

#include <stdbool.h>


/**
 * The variables one LIST VAR is read for.
 *
 * Negative means "the UPS did not report it", which is not the same as
 * zero: a charge of 0 is a flat battery, and a charge of -1 is a driver
 * that does not say.
 */
struct ups_reading {
    const char *status;		/**< ups.status, e.g. "OL CHRG"		*/
    double	charge;		/**< battery.charge, percent		*/
    double	runtime;	/**< battery.runtime, seconds		*/
    double	capacity;	/**< battery.capacity, Ah		*/
    double	current;	/**< battery.current, A, < 0 discharging */
};


/**
 * Whether the UPS is running off its battery.
 *
 * ups.status is a space-separated set of flags; OB is on battery and LB
 * is low battery. Matched as whole words: a substring search for "OB"
 * also finds it inside other flags, and NUT has several.
 *
 * False for a NULL or empty status, which means "not known to be on
 * battery" rather than "known to be on mains" -- the caller separates
 * those two by whether it got a reading at all.
 */
bool ups_on_battery(const char *status);

/**
 * How long the battery has left, in seconds, or < 0 when it cannot be
 * said. Sets `*estimated` when the answer was computed here rather than
 * reported by the UPS.
 *
 * Answered from battery.runtime whenever the driver reports it. Failing
 * that, and only while actually on battery, it is what is left of the
 * pack over what is leaving it:
 *
 *     charge/100 * capacity / -current, in hours
 *
 * A charging pack gets no answer: remaining time has no meaning there,
 * and printing one would be a number that looks like a measurement.
 * Neither does a driver that reports no capacity, no charge, or a
 * current with no sign -- a missing figure gets no answer rather than
 * a guess.
 *
 * `estimated` must not be NULL.
 */
double ups_runtime(const struct ups_reading *r, bool *estimated);

#endif
