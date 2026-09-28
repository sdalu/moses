/*
 * moses_display -- reading what a producer sent
 *
 * See payload.h. Pure string and number work, no I/O and no transport,
 * so test/test_payload.c links it on its own.
 */

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "payload.h"


bool
payload_text(const char *data, size_t len, char *out, size_t outlen)
{
    if ((data == NULL) || (out == NULL) || (outlen == 0))
	return false;
    if ((len == 0) || (len >= outlen))
	return false;
    if (memchr(data, '\0', len) != NULL)
	return false;

    memcpy(out, data, len);
    out[len] = '\0';
    return true;
}


bool
payload_double(const char *data, size_t len, double *val)
{
    char  buf[PAYLOAD_MAX];
    char *end;

    if (! payload_text(data, len, buf, sizeof(buf)))
	return false;

    double v = strtod(buf, &end);
    if (end == buf)			/* nothing numeric at all	*/
	return false;

    while ((*end == ' ') || (*end == '\t') || (*end == '\r') || (*end == '\n'))
	end++;
    if ((*end != '\0') || (! isfinite(v)))
	return false;

    *val = v;
    return true;
}


bool
payload_ulong(const char *data, size_t len, unsigned long *val)
{
    double v;

    if (! payload_double(data, len, &v))
	return false;
    if ((v < 0) || (v > (double)ULONG_MAX) || (v != floor(v)))
	return false;

    *val = (unsigned long)v;
    return true;
}


bool
payload_json_number(const char *data, size_t len, const char *key, double *val)
{
    char buf[PAYLOAD_MAX];
    char quoted[64];

    if ((key == NULL) || (! payload_text(data, len, buf, sizeof(buf))))
	return false;

    int n = snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    if ((n < 0) || ((size_t)n >= sizeof(quoted)))
	return false;

    const char *at = strstr(buf, quoted);
    if (at == NULL)
	return false;

    at = strchr(at + n, ':');
    if (at == NULL)
	return false;

    char  *end;
    double v = strtod(at + 1, &end);
    if ((end == at + 1) || (! isfinite(v)))
	return false;

    *val = v;
    return true;
}


bool
payload_json_string(const char *data, size_t len, const char *key,
		    char *out, size_t outlen)
{
    char buf[PAYLOAD_MAX];
    char quoted[64];

    if ((key == NULL) || (out == NULL) || (outlen == 0) ||
	(! payload_text(data, len, buf, sizeof(buf))))
	return false;

    int n = snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    if ((n < 0) || ((size_t)n >= sizeof(quoted)))
	return false;

    const char *at = strstr(buf, quoted);
    if (at == NULL)
	return false;

    at = strchr(at + n, ':');
    if (at == NULL)
	return false;
    for (at++ ; (*at == ' ') || (*at == '\t') ; at++)
	;
    if (*at != '"')
	return false;
    at++;

    const char *end = strchr(at, '"');
    if (end == NULL)
	return false;
    size_t vlen = (size_t)(end - at);
    if ((vlen >= outlen) || (memchr(at, '\\', vlen) != NULL))
	return false;

    memcpy(out, at, vlen);
    out[vlen] = '\0';
    return true;
}

bool
payload_online(const char *data, size_t len, bool *online)
{
    char buf[PAYLOAD_MAX];

    if (! payload_text(data, len, buf, sizeof(buf)))
	return false;

    *online = (strcasecmp(buf, "online") == 0);
    return true;
}
