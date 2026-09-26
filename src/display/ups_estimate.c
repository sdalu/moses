/*
 * moses_display -- what a UPS reading means
 *
 * See ups_estimate.h. Pure arithmetic and string work, no I/O, so
 * test/test_ups_estimate.c can link it on its own.
 */

#include <stdio.h>
#include <string.h>

#include "ups_estimate.h"


/* Longest ups.status worth copying to pick apart. NUT's flags are a
 * handful of two- and three-letter words; this is room for a dozen. */
#define STATUS_MAX	64

/* Seconds in an hour, spelled out where the estimate divides by it. */
#define PER_HOUR	3600.0


bool
ups_on_battery(const char *status)
{
    char  copy[STATUS_MAX];
    char *save = NULL;

    if ((status == NULL) || (status[0] == '\0'))
	return false;

    snprintf(copy, sizeof(copy), "%s", status);
    for (char *tok = strtok_r(copy, " ", &save) ; tok != NULL ;
	 tok = strtok_r(NULL, " ", &save)) {
	if ((strcmp(tok, "OB") == 0) || (strcmp(tok, "LB") == 0))
	    return true;
    }
    return false;
}


double
ups_runtime(const struct ups_reading *r, bool *estimated)
{
    *estimated = false;

    /* What the UPS says, whenever it says anything. */
    if (r->runtime >= 0)
	return r->runtime;

    if (! ups_on_battery(r->status))
	return -1;

    /* Otherwise: what is left of the pack, over what is leaving it.
     * Written as `! (current < 0)` rather than `current >= 0` so that a
     * NaN -- which is what a driver reporting no current at all leaves
     * here -- fails the test instead of passing it. */
    if ((r->charge < 0) || (r->capacity <= 0) || (! (r->current < 0)))
	return -1;

    *estimated = true;
    return (r->charge / 100.0) * r->capacity / -r->current * PER_HOUR;
}
