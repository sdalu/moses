/*
 * leak.c -- leak signatures in a stream of litres
 *
 * See leak.h for the three rules. The thresholds are not guesses: they
 * were set by replaying 2.3 years of one household's meter (1 L, one
 * sample a minute) against the leaks it actually had -- a toilet fill
 * valve that stuck open at 5.6 L/min, and a pipe that dripped 2.6 to
 * 6 L/h for three months. The replay and what each setting would have
 * done are in docs/leak.md, *Where the numbers come from*.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "leak.h"


//== Report ============================================================

static bool
set_report(struct leak *l, enum leak_level level, enum leak_kind kind,
	   double since, unsigned long litres, double rate)
{
    struct leak_report r = {
	.level = level, .kind = kind, .since = since,
	.litres = litres, .rate = rate,
    };
    struct leak_report *o = &l->report;

    // A running count or a drifting rate is not news; a new level, a
    // new kind or a new start is.
    bool changed = (o->level != r.level) || (o->kind != r.kind) ||
		   (o->since != r.since);
    l->report = r;
    return changed;
}


//== Slow ==============================================================

static int
cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void
slow_reset(struct leak *l)
{
    l->run_len   = 0;
    l->run_total = 0;
}

// An isolated litre at `t` joins the run.
static void
slow_append(struct leak *l, double t)
{
    if ((l->run_len > 0) && (t - l->run[l->run_len - 1] > LEAK_SLOW_MAXGAP))
	slow_reset(l);
    if (l->run_total == 0)
	l->run_first = t;
    l->run_total++;

    if (l->run_len == LEAK_SLOW_MAX) {
	memmove(l->run, l->run + 1, (LEAK_SLOW_MAX - 1) * sizeof(double));
	l->run_len--;
    }
    l->run[l->run_len++] = t;
}

// Whether the last `slow` litres of the run are evenly spaced; the
// median interval goes to *interval.
static bool
slow_even(const struct leak *l, double *interval)
{
    unsigned n = l->cfg.slow;
    if ((n < 3) || (l->run_len < n))
	return false;

    const double *t = l->run + l->run_len - n;
    double iv[LEAK_SLOW_MAX];
    double sum = 0, sq = 0;
    for (unsigned i = 1 ; i < n ; i++) {
	iv[i - 1] = t[i] - t[i - 1];
	sum      += iv[i - 1];
    }
    double mean = sum / (n - 1);
    for (unsigned i = 0 ; i < n - 1 ; i++)
	sq += (iv[i] - mean) * (iv[i] - mean);
    double cv = sqrt(sq / (n - 1)) / mean;

    qsort(iv, n - 1, sizeof(double), cmp_double);
    *interval = ((n - 1) % 2) ? iv[(n - 1) / 2]
			      : (iv[(n - 1) / 2 - 1] + iv[(n - 1) / 2]) / 2;
    return cv <= LEAK_SLOW_CV;
}


//== Evaluation ========================================================

// Highest level wins; flow before slow before quiet at equal level.
static bool
evaluate(struct leak *l, double now)
{
    const struct leak_config *c = &l->cfg;

    // flow: still flowing if the last litre is recent enough
    if ((c->flow > 0) && (l->last >= 0) && (now - l->last <= LEAK_FLOW_GAP) &&
	(l->last - l->flow_start >= c->flow)) {
	double mins = (l->last - l->flow_start) / 60.0;
	return set_report(l, LEAK_ALERT, LEAK_FLOW, l->flow_start,
			  l->flow_litres, mins > 0 ? l->flow_litres / mins : 0);
    }

    // slow
    double interval;
    if ((c->slow > 0) && slow_even(l, &interval)) {
	// Since the run began and all its litres: fixed while the drip
	// goes on, so each new litre is not a new report.
	return set_report(l, LEAK_ALERT, LEAK_SLOW, l->run_first, l->run_total,
			  3600.0 / interval);
    }

    // quiet
    if ((c->quiet > 0) && (now - l->quiet_seen > LEAK_QUIET_WINDOW))
	return set_report(l, LEAK_WARN, LEAK_QUIET, l->quiet_seen, 0, 0);

    return set_report(l, LEAK_OK, LEAK_NONE, 0, 0, 0);
}

// Time moving on: settle what only waiting can settle.
static void
advance(struct leak *l, double now)
{
    // A pending litre with nothing around it for LEAK_ISOLATION is
    // isolated, and joins the run once.
    if ((l->pend >= 0) && !l->pend_clustered && !l->pend_done &&
	(now - l->pend >= LEAK_ISOLATION)) {
	slow_append(l, l->pend);
	l->pend_done = true;
    }

    // An ongoing quiet stretch long enough counts as seen, now.
    double from = (l->last >= 0) ? l->last : l->started;
    if ((l->cfg.quiet > 0) && (now - from >= l->cfg.quiet))
	l->quiet_seen = now;
}


//== Interface =========================================================

void
leak_init(struct leak *l, const struct leak_config *cfg, double now)
{
    memset(l, 0, sizeof(*l));
    l->cfg        = *cfg;
    l->last       = -1;
    l->pend       = -1;
    l->started    = now;
    l->quiet_seen = now;          // no warning for the first day
}

bool
leak_litres(struct leak *l, double t, unsigned n)
{
    if (n == 0)
	return leak_tick(l, t);

    advance(l, t);

    // quiet: the stretch that ends here may have been long enough
    double from = (l->last >= 0) ? l->last : l->started;
    if ((l->cfg.quiet > 0) && (t - from >= l->cfg.quiet))
	l->quiet_seen = t;

    // flow
    if ((l->last < 0) || (t - l->last > LEAK_FLOW_GAP)) {
	l->flow_start  = t;
	l->flow_litres = 0;
    }
    l->flow_litres += n;

    // slow: a litre near the pending one makes both a cluster, and a
    // cluster breaks the run
    // (a pending litre is only taken into the run after LEAK_ISOLATION,
    // so one this near has not been)
    bool near = (l->pend >= 0) && (t - l->pend < LEAK_ISOLATION);
    if (near || (n > 1)) {
	slow_reset(l);
	l->pend_clustered = true;
    } else {
	l->pend_clustered = false;
    }
    l->pend      = t;
    l->pend_done = false;
    l->last      = t;

    return evaluate(l, t);
}

bool
leak_tick(struct leak *l, double t)
{
    advance(l, t);
    return evaluate(l, t);
}

const struct leak_report *
leak_report(const struct leak *l)
{
    return &l->report;
}

const char *
leak_level_name(enum leak_level level)
{
    switch (level) {
    case LEAK_OK:    return "ok";
    case LEAK_WARN:  return "warn";
    case LEAK_ALERT: return "alert";
    }
    return "?";
}

const char *
leak_kind_name(enum leak_kind kind)
{
    switch (kind) {
    case LEAK_NONE:  return "none";
    case LEAK_FLOW:  return "flow";
    case LEAK_SLOW:  return "slow";
    case LEAK_QUIET: return "quiet";
    }
    return "?";
}


//== Pulse check =======================================================

void
pulse_check_init(struct pulse_check *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->why, sizeof(c->why), "not yet compared with the index");
}

bool
pulse_check_reading(struct pulse_check *c, unsigned litres, unsigned pulses)
{
    enum pulse_health before = c->health;

    c->litres += litres;
    c->pulses += pulses;
    if (litres > 0)
	c->readings++;

    bool verdict, match;
    if ((c->litres == 0) && (c->pulses >= PULSE_CHECK_NOISE)) {
	verdict = true; match = false;
	snprintf(c->why, sizeof(c->why), "%lu pulses while the index did not move",
		 c->pulses);
    } else if ((c->readings >= PULSE_CHECK_READINGS) &&
	       (c->litres   >= PULSE_CHECK_LITRES)) {
	unsigned long slack = PULSE_CHECK_SLACK + c->litres / 10;
	unsigned long diff  = (c->pulses > c->litres) ? c->pulses - c->litres
						      : c->litres - c->pulses;
	verdict = true; match = (diff <= slack);
	if (c->pulses == 0)
	    snprintf(c->why, sizeof(c->why), "no pulse for %lu L of index",
		     c->litres);
	else
	    snprintf(c->why, sizeof(c->why), "%lu pulses for %lu L of index",
		     c->pulses, c->litres);
    } else {
	verdict = false; match = false;
    }

    if (verdict) {
	if (match) {
	    c->good++;
	    if ((c->health == PULSE_UNKNOWN) ||
		((c->health == PULSE_BROKEN) && (c->good >= PULSE_CHECK_RECOVER)))
		c->health = PULSE_OK;
	} else {
	    c->good   = 0;
	    c->health = PULSE_BROKEN;
	}
	c->litres = c->pulses = 0;
	c->readings = 0;
    }
    return c->health != before;
}

const char *
pulse_health_name(enum pulse_health health)
{
    switch (health) {
    case PULSE_UNKNOWN: return "unknown";
    case PULSE_OK:      return "ok";
    case PULSE_BROKEN:  return "broken";
    }
    return "?";
}
