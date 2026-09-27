/*
 * moses_display -- reading the line protocol off the local socket
 *
 * See lineproto.h for the wire and for what is deliberately not
 * handled. This is a scan rather than a parser generator, for the reason
 * payload_json_number() is one: the producers are three files in this
 * same tree, their format strings are fixed, and the whole grammar that
 * has to be read is a name, some key=value pairs and a number to skip.
 *
 * It refuses rather than guesses, everywhere. A line that is not exactly
 * the shape above yields nothing at all, instead of the fields it
 * managed to read before it stopped making sense -- half a line is how
 * a wrong figure gets onto the panel, and a dash is better than that.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "breaker_state.h"
#include "lineproto.h"
#include "payload.h"


/*
 * Whether a token is the nanosecond timestamp: digits, and at least one.
 *
 * Checked rather than skipped, and this is load-bearing. Fields are
 * split on the first space after the measurement, so a space anywhere
 * inside them -- a quoted value containing one, a tag set, a producer
 * that is not one of ours -- leaves a "timestamp" that is not a number.
 * Refusing it there is what stops the half of a line before the space
 * being read as though it were all of it.
 */
static bool
is_timestamp(const char *s)
{
    if (*s == '\0')
	return false;

    for ( ; *s != '\0' ; s++) {
	if (! isdigit((unsigned char)*s))
	    return false;
    }
    return true;
}


/*
 * One `key=value`, for a known measurement. Returns the kind, having
 * filled in whatever that kind carries.
 *
 * Unknown keys are LINEPROTO_IGNORED rather than an error: a producer
 * that starts reporting something new must not stop this one reading
 * what it already understood.
 */
static enum lineproto_kind
field(const char *measurement, const char *key, const char *value,
      struct lineproto_reading *out)
{
    size_t vlen = strlen(value);

    /* A quoted value. `failure` is the only one the producers send, and
     * the quotes are required: put_fail() emits them because line
     * protocol refuses a bare string, so a bare one here did not come
     * from it. */
    if (value[0] == '"') {
	if ((vlen < 2) || (value[vlen - 1] != '"'))
	    return LINEPROTO_IGNORED;

	if (strcmp(key, "failure") != 0)
	    return LINEPROTO_IGNORED;

	size_t n = vlen - 2;
	if (n >= sizeof(out->failure))
	    n = sizeof(out->failure) - 1;
	memcpy(out->failure, value + 1, n);
	out->failure[n] = '\0';
	return LINEPROTO_FAILURE;
    }

    if (strcmp(measurement, LINEPROTO_WATERMETER) == 0) {
	if (strcmp(key, "index") == 0) {
	    if (! payload_double(value, vlen, &out->litres))
		return LINEPROTO_IGNORED;
	    return LINEPROTO_INDEX;
	}
	if (strcmp(key, "pulse") == 0) {
	    if (! payload_ulong(value, vlen, &out->count))
		return LINEPROTO_IGNORED;
	    return LINEPROTO_PULSE;
	}
	return LINEPROTO_IGNORED;
    }

    if (strcmp(measurement, LINEPROTO_ENVIRONMENT) == 0) {
	/* pressure and humidity are on the wire and not on the panel. */
	if (strcmp(key, "temperature") == 0) {
	    if (! payload_double(value, vlen, &out->celsius))
		return LINEPROTO_IGNORED;
	    return LINEPROTO_TEMPERATURE;
	}
	return LINEPROTO_IGNORED;
    }

    if (strcmp(measurement, LINEPROTO_BREAKER) == 0) {
	if (strcmp(key, "state") == 0) {
	    /* breaker_parse_state() rather than a second reading of the
	     * same vocabulary -- it is what moses_breaker reads its own
	     * commands with, and what source-mqtt.c uses for the `state`
	     * topic. One parser, one meaning of "1". */
	    int state = breaker_parse_state(value, (int)vlen);
	    if (state < 0)
		return LINEPROTO_IGNORED;
	    out->closed = (state != 0);
	    return LINEPROTO_VALVE;
	}
	return LINEPROTO_IGNORED;
    }

    return LINEPROTO_IGNORED;
}


size_t
lineproto_parse(const char *data, size_t len,
		struct lineproto_reading *out, size_t max, bool *ok)
{
    if (ok != NULL)
	*ok = false;

    if ((data == NULL) || (out == NULL) || (max == 0))
	return 0;

    /* NUL-terminate a copy, so the scan below can use string functions.
     * payload_text() is what does it, for the refusals it already makes:
     * an empty line, one too long to be from these producers, and one
     * with a NUL inside -- that last because every strcmp() below would
     * otherwise read a shorter line that happens to parse. */
    char line[LINEPROTO_MAX];
    if (! payload_text(data, len, line, sizeof(line)))
	return 0;

    /* The stdout sink ends its line with a newline and the socket sink
     * does not. Accept either, and a CRLF while here. */
    size_t n = strlen(line);
    while ((n > 0) && ((line[n - 1] == '\n') || (line[n - 1] == '\r')))
	line[--n] = '\0';
    if (n == 0)
	return 0;

    /* <measurement> <fields> [<timestamp>] */
    char *sp = strchr(line, ' ');
    if (sp == NULL)
	return 0;
    *sp = '\0';

    const char *measurement = line;
    char       *fields      = sp + 1;

    /* A measurement with a tag set is not something these producers
     * emit, and its fields would not mean what this assumes. */
    if (strchr(measurement, ',') != NULL)
	return 0;
    if (*measurement == '\0')
	return 0;

    sp = strchr(fields, ' ');
    if (sp != NULL) {
	*sp = '\0';
	if (! is_timestamp(sp + 1))
	    return 0;
    }
    if (*fields == '\0')
	return 0;

    /* Every field, comma separated. */
    size_t count = 0;
    char  *save  = NULL;
    for (char *tok = strtok_r(fields, ",", &save) ; tok != NULL ;
	 tok = strtok_r(NULL, ",", &save)) {

	char *eq = strchr(tok, '=');
	if ((eq == NULL) || (eq == tok))
	    return 0;			/* not key=value: not this line */
	*eq = '\0';

	if (count >= max)
	    break;			/* room ran out; what is read stands */

	struct lineproto_reading r;
	memset(&r, 0, sizeof(r));
	snprintf(r.measurement, sizeof(r.measurement), "%s", measurement);

	enum lineproto_kind kind = field(measurement, tok, eq + 1, &r);
	if (kind == LINEPROTO_IGNORED)
	    continue;

	r.kind   = kind;
	out[count++] = r;
    }

    if (ok != NULL)
	*ok = true;
    return count;
}
