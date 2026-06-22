/*
 * breaker_parse_state -- interpret the breaker setter payload.
 *
 * Kept in its own translation unit (separate from breaker.c, which has
 * main()) so it can be linked into the unit tests.
 */

#include <string.h>
#include <strings.h>

#include "breaker_state.h"

#define BREAKER_STATE(n, v) { n, sizeof(n) - 1, v }
static const struct breaker_state {
    const char *name;
    int         namelen;
    int         value;
} breaker_state[] = {
    BREAKER_STATE("0",     0), BREAKER_STATE("1",    1),
    BREAKER_STATE("false", 0), BREAKER_STATE("true", 1),
    BREAKER_STATE("off",   0), BREAKER_STATE("on",   1),
};

int
breaker_parse_state(const char *data, int datalen)
{
    // Reject a negative length or an embedded NUL, so the strncasecmp
    // comparisons below stay well-defined against the fixed names.
    if (datalen < 0)
	return -1;
    if (strnlen(data, datalen) != (size_t)datalen)
	return -1;

    for (size_t i = 0 ; i < sizeof(breaker_state) / sizeof(breaker_state[0]) ; i++)
	if ((breaker_state[i].namelen == datalen) &&
	    (! strncasecmp(breaker_state[i].name, data, datalen)))
	    return breaker_state[i].value;
    return -1;
}
