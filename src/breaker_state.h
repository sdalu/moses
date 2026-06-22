#ifndef __BREAKER_STATE_H
#define __BREAKER_STATE_H

/*
 * Parse a breaker state command payload, as received on the MQTT setter
 * topic. Accepts 0/1, off/on, false/true (case insensitive). The match is
 * exact: leading/trailing whitespace is not trimmed and the length is
 * honoured (so "on" with datalen 1 is "o", not a match).
 *
 * Returns 1 or 0 for the requested state, or -1 if the payload is not
 * recognized (including a negative length or an embedded NUL).
 */
int breaker_parse_state(const char *data, int datalen);

#endif
