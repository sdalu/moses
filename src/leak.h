/*
 * leak.h -- leak signatures in a stream of litres
 *
 * Pure logic: litres go in with the time they were counted, a report
 * comes out. No clock is read, nothing is published, nothing is locked;
 * moses_watermeter does those (src/watermeter.c), which is what lets
 * test/test_leak.c replay a day in microseconds.
 *
 * Three rules, each off when its setting is 0:
 *
 *   flow    Water has not stopped for `flow` seconds: no two litres
 *           further apart than LEAK_FLOW_GAP. A toilet whose fill valve
 *           does not close, a burst pipe, a tap left open. Alert.
 *
 *   slow    `slow` isolated litres in a row, evenly spaced. A litre is
 *           isolated when no other one is counted within LEAK_ISOLATION
 *           of it: a drip gives isolated litres, a tap or a flush gives
 *           clusters, and a cluster starts the run again. Evenly means
 *           the intervals' coefficient of variation is at most
 *           LEAK_SLOW_CV. Alert.
 *
 *   quiet   The last 24 hours held no stretch of `quiet` seconds
 *           without a litre. A house without a leak always has one.
 *           Warning: it is the weakest of the three.
 *
 * The report is the current evidence, not a latch: a flow alert ends
 * when the water stops, a slow alert when a cluster breaks the run.
 * Whether to close the valve is not decided here, nor anywhere in
 * moses (DESIGN.md, *Leaks are reported, not acted on*).
 */

#ifndef __LEAK_H
#define __LEAK_H

#include <stdbool.h>

#define LEAK_FLOW_GAP     90.0     // s: a longer gap ends a flow
#define LEAK_ISOLATION   180.0     // s: no other litre within this
#define LEAK_SLOW_CV       0.25    // evenness of a slow run
#define LEAK_SLOW_MAXGAP  (3 * 3600.0) // s: a longer interval ends a run
#define LEAK_QUIET_WINDOW (24 * 3600.0) // s: where a quiet stretch is looked for
#define LEAK_SLOW_MAX     64       // longest run kept

enum leak_level { LEAK_OK = 0, LEAK_WARN = 1, LEAK_ALERT = 2 };
enum leak_kind  { LEAK_NONE = 0, LEAK_FLOW, LEAK_SLOW, LEAK_QUIET };

struct leak_config {
    double   flow;                 // s, 0 = off
    unsigned slow;                 // litres, 0 = off
    double   quiet;                // s, 0 = off
};

struct leak_report {
    enum leak_level level;
    enum leak_kind  kind;
    double          since;         // when the signature started
    unsigned long   litres;        // counted since then (flow, slow)
    double          rate;          // L/min (flow), L/h (slow), 0 (quiet)
};

struct leak {
    struct leak_config cfg;

    double        last;            // last litre, < 0 before the first
    double        started;         // leak_init() time, for quiet

    // flow
    double        flow_start;
    unsigned long flow_litres;

    // slow
    double        pend;            // the litre whose isolation is pending
    bool          pend_clustered;
    bool          pend_done;       // already taken into the run
    double        run[LEAK_SLOW_MAX];  // the latest lone litres of the run
    unsigned      run_len;
    double        run_first;           // the run's first lone litre
    unsigned long run_total;           // lone litres in the run, all of them

    // quiet
    double        quiet_seen;      // latest time a quiet stretch was seen

    struct leak_report report;
};

/** Start with nothing counted, at time `now`. */
void leak_init(struct leak *l, const struct leak_config *cfg, double now);

/** `n` litres counted at time `t` (seconds, any clock, never going
 *  back). Returns true when the report changed. */
bool leak_litres(struct leak *l, double t, unsigned n);

/** Time passing with nothing counted; call it every few tens of
 *  seconds. Returns true when the report changed. */
bool leak_tick(struct leak *l, double t);

const struct leak_report *leak_report(const struct leak *l);

const char *leak_level_name(enum leak_level level);
const char *leak_kind_name(enum leak_kind kind);


/*
 * Whether the pulses can be believed, judged against the index.
 *
 * Both count the same water, so between index readings the pulses
 * counted should match what the index grew by. They are compared in
 * windows of at least PULSE_CHECK_READINGS readings that grew and
 * PULSE_CHECK_LITRES litres, with slack for a pulse that lands on the
 * other side of a reading: PULSE_CHECK_SLACK plus 10 %. A window that
 * does not match -- no pulse at all, too many, too few -- makes the
 * pulses broken; so does PULSE_CHECK_NOISE pulses while the index did
 * not move. Broken pulses are believed again after PULSE_CHECK_RECOVER
 * matching windows in a row, so a line that is only sometimes wrong
 * does not flap.
 */

#define PULSE_CHECK_READINGS 3
#define PULSE_CHECK_LITRES   10
#define PULSE_CHECK_SLACK    2
#define PULSE_CHECK_NOISE    10
#define PULSE_CHECK_RECOVER  2

enum pulse_health { PULSE_UNKNOWN = 0, PULSE_OK, PULSE_BROKEN };

struct pulse_check {
    enum pulse_health health;
    unsigned long     litres;     // index growth in the current window
    unsigned long     pulses;     // pulses in the current window
    unsigned          readings;   // readings that grew, in the window
    unsigned          good;       // matching windows in a row
    char              why[96];    // the last verdict, in words
};

void pulse_check_init(struct pulse_check *c);

/** One index reading: `litres` it grew by, `pulses` counted since the
 *  reading before. Returns true when the health changed. */
bool pulse_check_reading(struct pulse_check *c, unsigned litres, unsigned pulses);

const char *pulse_health_name(enum pulse_health health);

#endif
