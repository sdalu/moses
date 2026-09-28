/*
 * Unit tests for src/leak.c -- the leak signatures moses_watermeter
 * reports on the `leak` topic.
 *
 * Synthetic streams check each rule's edges; three replays of the real
 * meter (test/leak_traces.h) check that the thresholds still catch the
 * leaks they were set on and stay quiet on an ordinary day.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "leak.h"

struct minute { unsigned m, litres; };
#include "leak_traces.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
	    failures++;							\
	    fprintf(stderr, "FAIL %s:%d: %s\n",				\
		    __FILE__, __LINE__, #cond);				\
	}								\
    } while (0)

static const struct leak_config ALL = { .flow = 20 * 60, .slow = 8,
					.quiet = 2 * 3600 };

// What a stream did: the worst report seen, and when it first came.
struct outcome {
    enum leak_level level;
    enum leak_kind  kind;
    double          first;     // time the worst level was first reached
    double          rate;
};

static void
seen(struct outcome *o, const struct leak *l, double t)
{
    const struct leak_report *r = leak_report(l);
    if (r->level > o->level) {
	o->level = r->level; o->kind = r->kind; o->first = t; o->rate = r->rate;
    }
}

// Litres at given times (seconds), ticking every 30 s in between.
static struct outcome
run(const struct leak_config *cfg, const double *t, const unsigned *n,
    unsigned count, double end)
{
    struct leak l;
    struct outcome o = { LEAK_OK, LEAK_NONE, -1, 0 };
    leak_init(&l, cfg, 0);
    double clock = 0;
    for (unsigned i = 0 ; i < count ; i++) {
	for ( ; clock + 30 < t[i] ; clock += 30) {
	    leak_tick(&l, clock + 30); seen(&o, &l, clock + 30);
	}
	leak_litres(&l, t[i], n[i]); seen(&o, &l, t[i]);
	clock = t[i];
    }
    for ( ; clock + 30 <= end ; clock += 30) {
	leak_tick(&l, clock + 30); seen(&o, &l, clock + 30);
    }
    return o;
}

// A replay: each minute's litres counted at its middle, as the M-Bus
// index delivers them.
static struct outcome
replay(const struct leak_config *cfg, const struct minute *tr, unsigned count,
       unsigned minutes)
{
    double   *t = calloc(count, sizeof(double));
    unsigned *n = calloc(count, sizeof(unsigned));
    for (unsigned i = 0 ; i < count ; i++) {
	t[i] = tr[i].m * 60.0 + 30; n[i] = tr[i].litres;
    }
    struct outcome o = run(cfg, t, n, count, minutes * 60.0);
    free(t); free(n);
    return o;
}

#define REPLAY(cfg, tr) replay(cfg, tr, sizeof(tr) / sizeof(tr[0]), tr##_minutes)

int
main(void)
{
    static double   t[4000];
    static unsigned n[4000];
    unsigned k;
    struct outcome o;

    // flow: 5.6 L/min, one litre every ~10.7 s, for 25 min
    k = 0;
    for (double s = 0 ; s < 25 * 60 ; s += 60.0 / 5.6) { t[k] = 100 + s; n[k++] = 1; }
    o = run(&ALL, t, n, k, 100 + 30 * 60);
    CHECK(o.level == LEAK_ALERT);
    CHECK(o.kind  == LEAK_FLOW);
    CHECK(o.first >= 100 + 20 * 60 - 11 && o.first <= 100 + 20 * 60 + 11);
    CHECK(o.rate  > 5.3 && o.rate < 5.9);

    // ... and 19 minutes of it is not enough
    k = 0;
    for (double s = 0 ; s < 19 * 60 ; s += 60.0 / 5.6) { t[k] = 100 + s; n[k++] = 1; }
    o = run(&ALL, t, n, k, 100 + 30 * 60);
    CHECK(o.level == LEAK_OK);

    // the flow alert ends when the water stops
    {
	struct leak l; leak_init(&l, &ALL, 0);
	for (unsigned i = 0 ; i <= 25 * 60 ; i += 10) leak_litres(&l, i, 1);
	CHECK(leak_report(&l)->kind == LEAK_FLOW);
	CHECK(leak_tick(&l, 25 * 60 + LEAK_FLOW_GAP + 1));   // changed
	CHECK(leak_report(&l)->level == LEAK_OK);
    }

    // an on/off shower -- 2 min on, 3 min off, 5 times -- is not a flow
    k = 0;
    for (int b = 0 ; b < 5 ; b++)
	for (double s = 0 ; s < 120 ; s += 8) { t[k] = 60 + b * 300 + s; n[k++] = 1; }
    o = run(&ALL, t, n, k, 3600);
    CHECK(o.level == LEAK_OK);

    // slow: a litre every 20 min, 3 L/h
    k = 0;
    for (int i = 0 ; i < 10 ; i++) { t[k] = 600 + i * 1200.0 + (i % 2 ? 40 : -40); n[k++] = 1; }
    o = run(&ALL, t, n, k, 600 + 11 * 1200.0);
    CHECK(o.level == LEAK_ALERT);
    CHECK(o.kind  == LEAK_SLOW);
    CHECK(o.rate  > 2.7 && o.rate < 3.3);
    // the eighth litre is known isolated LEAK_ISOLATION after it lands
    CHECK(o.first >= t[7] + LEAK_ISOLATION && o.first <= t[7] + LEAK_ISOLATION + 30);

    // ... reported once, not once per litre: `since` is the run's start
    {
	struct leak l; leak_init(&l, &ALL, 0);
	unsigned changes = 0; double clock = 0;
	for (int i = 0 ; i < 10 ; i++) {
	    double at = 600 + i * 1200.0;
	    for ( ; clock + 30 < at ; clock += 30) changes += leak_tick(&l, clock + 30);
	    changes += leak_litres(&l, at, 1); clock = at;
	}
	for ( ; clock < 600 + 10 * 1200.0 ; clock += 30) changes += leak_tick(&l, clock + 30);
	CHECK(changes == 1);
	CHECK(leak_report(&l)->kind == LEAK_SLOW);
	CHECK(leak_report(&l)->since == 600);
	CHECK(leak_report(&l)->litres == 10);
    }

    // ... seven of them are not enough
    o = run(&ALL, t, n, 7, t[6] + 3600);
    CHECK(o.level == LEAK_OK);

    // ... and a flush in the middle starts the run again
    {
	unsigned m = 0; double tt[20]; unsigned nn[20];
	for (int i = 0 ; i < 6 ; i++) { tt[m] = 600 + i * 1200.0; nn[m++] = 1; }
	tt[m] = 600 + 5 * 1200.0 + 300; nn[m++] = 8;          // a flush
	for (int i = 6 ; i < 12 ; i++) { tt[m] = 600 + i * 1200.0; nn[m++] = 1; }
	o = run(&ALL, tt, nn, m, 600 + 13 * 1200.0);
	CHECK(o.level == LEAK_OK);
    }

    // ... irregular isolated litres are not a drip
    {
	static const double gaps[] = { 400, 3000, 900, 5000, 700, 2500, 1200, 6000, 500 };
	k = 0; double s = 300;
	for (unsigned i = 0 ; i < sizeof(gaps) / sizeof(gaps[0]) ; i++) {
	    t[k] = s; n[k++] = 1; s += gaps[i];
	}
	o = run(&ALL, t, n, k, s + 3600);
	CHECK(o.level == LEAK_OK);
    }

    // quiet: a litre every 90 min never leaves 2 h of quiet
    {
	struct leak_config c = { .quiet = 2 * 3600 };
	k = 0;
	for (double s = 0 ; s < 30 * 3600 ; s += 90 * 60) { t[k] = s + 60; n[k++] = 3; }
	o = run(&c, t, n, k, 30 * 3600);
	CHECK(o.level == LEAK_WARN);
	CHECK(o.kind  == LEAK_QUIET);
	CHECK(o.first > 24 * 3600);                            // never on day one

	// ... while a 5 h night does
	k = 0;
	for (double s = 0 ; s < 30 * 3600 ; s += 90 * 60)
	    if (fmod(s, 24 * 3600) < 19 * 3600) { t[k] = s + 60; n[k++] = 3; }
	o = run(&c, t, n, k, 30 * 3600);
	CHECK(o.level == LEAK_OK);
    }

    // a rule set to 0 is off
    {
	struct leak_config c = { .flow = 0, .slow = 8, .quiet = 0 };
	k = 0;
	for (unsigned i = 0 ; i <= 40 * 60 ; i += 10) { t[k] = i; n[k++] = 1; }
	o = run(&c, t, n, k, 50 * 60);
	CHECK(o.level == LEAK_OK);
    }

    // replays of the real meter
    o = REPLAY(&ALL, pipe_leak_night);
    CHECK(o.level == LEAK_ALERT);
    CHECK(o.kind  == LEAK_SLOW);
    CHECK(o.rate  > 2.5 && o.rate < 4.0);

    o = REPLAY(&ALL, toilet_running);
    CHECK(o.level == LEAK_ALERT);
    CHECK(o.kind  == LEAK_FLOW);
    CHECK(o.first <= (61 + 21) * 60.0);                     // 01:01 + 20 min

    o = REPLAY(&ALL, ordinary_day);
    CHECK(o.level == LEAK_OK);

    // pulse check: pulses matching the index are believed after one window
    {
	struct pulse_check pc; pulse_check_init(&pc);
	CHECK(pc.health == PULSE_UNKNOWN);
	CHECK(!pulse_check_reading(&pc, 4, 4));
	CHECK(!pulse_check_reading(&pc, 0, 0));      // a reading that did not grow
	CHECK(!pulse_check_reading(&pc, 3, 2));      // one pulse late ...
	CHECK( pulse_check_reading(&pc, 5, 6));      // ... caught up: 12 vs 12
	CHECK(pc.health == PULSE_OK);

	// no pulse at all while the index grows: broken, and said why
	for (int i = 0 ; i < 3 ; i++) pulse_check_reading(&pc, 4, 0);
	CHECK(pc.health == PULSE_BROKEN);
	CHECK(strstr(pc.why, "no pulse for 12 L") != NULL);

	// one matching window is not enough to believe them again, two are
	for (int i = 0 ; i < 3 ; i++) pulse_check_reading(&pc, 4, 4);
	CHECK(pc.health == PULSE_BROKEN);
	for (int i = 0 ; i < 3 ; i++) pulse_check_reading(&pc, 4, 4);
	CHECK(pc.health == PULSE_OK);

	// half the pulses (a divisor or an edge wrong): broken
	for (int i = 0 ; i < 3 ; i++) pulse_check_reading(&pc, 8, 4);
	CHECK(pc.health == PULSE_BROKEN);
	CHECK(strstr(pc.why, "12 pulses for 24 L") != NULL);
    }
    {
	// within the slack (2 + 10 %): 23 pulses for 20 L is a match
	struct pulse_check pc; pulse_check_init(&pc);
	pulse_check_reading(&pc, 7, 8); pulse_check_reading(&pc, 7, 8);
	pulse_check_reading(&pc, 6, 7);
	CHECK(pc.health == PULSE_OK);
	// 25 for 20 is not
	pulse_check_init(&pc);
	pulse_check_reading(&pc, 7, 9); pulse_check_reading(&pc, 7, 8);
	pulse_check_reading(&pc, 6, 8);
	CHECK(pc.health == PULSE_BROKEN);
	// a bouncing line: pulses with no water
	pulse_check_init(&pc);
	for (int i = 0 ; i < 5 ; i++) pulse_check_reading(&pc, 0, 2);
	CHECK(pc.health == PULSE_BROKEN);
	CHECK(strstr(pc.why, "index did not move") != NULL);
	// too little water to judge leaves it unknown
	pulse_check_init(&pc);
	for (int i = 0 ; i < 5 ; i++) pulse_check_reading(&pc, 1, 1);
	CHECK(pc.health == PULSE_UNKNOWN);
    }

    CHECK(strcmp(leak_level_name(LEAK_ALERT), "alert") == 0);
    CHECK(strcmp(leak_kind_name(LEAK_SLOW), "slow") == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
