/*
 * moses_display -- reading what a producer sent
 *
 * The payload parsers, kept apart from the transport that delivered
 * them. They decide what number ends up on the panel, so they are their
 * own translation unit and their own test (test/test_payload.c) -- the
 * same reason src/breaker_state.c is.
 *
 * Everything takes a pointer and a length rather than a string, because
 * that is what arrives: mosquitto hands over a buffer that is not
 * NUL-terminated, and a unix datagram would do the same. Nothing here
 * includes mosquitto, LVGL, or a header that is Linux-only, which is
 * what lets the datagram source of TODO, *A local socket*, reuse them
 * unchanged.
 *
 * All of them refuse rather than guess. A payload that is not exactly
 * what was expected returns false and leaves its output alone, because
 * a wrong figure on this screen is worse than a missing one -- a dash
 * says "not known", while a plausible number says something false.
 */

#ifndef __DISPLAY_PAYLOAD_H
#define __DISPLAY_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>


/** Longest payload any of these will look at. */
#define PAYLOAD_MAX	256


/**
 * Copy a payload out as a NUL-terminated string.
 *
 * Refuses an empty payload, one too long for `out`, and one with a NUL
 * inside it -- that last because every parse here is a string function,
 * and a payload that ends early would be read as a shorter one that
 * happens to parse.
 */
bool payload_text(const char *data, size_t len, char *out, size_t outlen);

/**
 * A payload that is one number and nothing else.
 *
 * Trailing whitespace is allowed, anything else after the number is
 * not. Non-finite values are refused: strtod() accepts "nan" and "inf",
 * and a meter index of NaN would reach the panel as "nan" -- a reading,
 * apparently, rather than the garbage it is.
 */
bool payload_double(const char *data, size_t len, double *val);

/**
 * A payload that is one whole, non-negative number.
 *
 * Fractions are refused rather than truncated: a pulse count of "3.7"
 * is not three, it is a producer that has gone wrong. Range-checked
 * before the conversion, because a double past ULONG_MAX converts to
 * unsigned long undefined -- and "1e30" is a well-formed thing for a
 * broken publisher to send.
 */
bool payload_ulong(const char *data, size_t len, unsigned long *val);

/**
 * One number out of a flat JSON object, by key.
 *
 * A deliberate scan rather than a JSON parser: the producer is
 * src/sensors.c in this same tree, its format string is fixed, and a
 * dependency for three floats would not pay for itself. It matches the
 * key with its quotes, so "temp" does not find "temperature", and it
 * steps over the colon rather than counting members, so it survives
 * them being reordered or added to.
 *
 * The limit of that shortcut: it finds the first occurrence of the
 * quoted key anywhere in the object, including inside a string value.
 * The payloads it is pointed at have no string values.
 */
bool payload_json_number(const char *data, size_t len, const char *key,
			 double *val);

/**
 * An availability payload: "online", or anything else.
 *
 * Case insensitive. Whatever is not "online" counts as offline, because
 * the retained last will is the only other thing published there.
 */
bool payload_online(const char *data, size_t len, bool *online);

#endif
