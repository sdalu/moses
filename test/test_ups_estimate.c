/*
 * Unit tests for the UPS reading interpretation -- ups_on_battery() and
 * ups_runtime().
 *
 * These decide what the front panel says about the battery, and one of
 * them is a division the UPS never performed: the pijuice driver moses
 * runs reports no battery.runtime, so the remaining time on the screen
 * is worked out from the charge, the pack capacity and the current. A
 * wrong answer here is a number that looks like a measurement and is
 * not, which is worse than no number at all -- hence the cases below
 * for every way the inputs can fail to add up.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ups_estimate.h"

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

/* Runtimes are seconds; a second either way is not what these test. */
#define NEAR(a, b)	(fabs((a) - (b)) < 1.0)

/* A reading with nothing reported, to be filled in per case. */
#define BLANK	{ .status = "", .charge = -1, .runtime = -1,		\
		  .capacity = -1, .current = NAN }


int
main(void)
{
    bool est;

    //== ups_on_battery ================================================

    // The two flags that mean it
    CHECK(ups_on_battery("OB")        == true);
    CHECK(ups_on_battery("LB")        == true);
    CHECK(ups_on_battery("OB LB")     == true);
    CHECK(ups_on_battery("OB DISCHRG") == true);

    // On mains, in the shapes NUT actually emits
    CHECK(ups_on_battery("OL")           == false);
    CHECK(ups_on_battery("OL CHRG")      == false);
    CHECK(ups_on_battery("HB OL CHRG")   == false);	// moses, charging

    // Whole words only. Each of these contains "OB" or "LB" inside a
    // longer token and none of them means on battery -- a plain
    // strstr() would call every one of them a power failure. Contrived
    // rather than drawn from NUT's flag list, which is the point: the
    // tokenizer is what makes the next flag someone adds safe.
    CHECK(ups_on_battery("OBOE")      == false);	// "OB" at the front
    CHECK(ups_on_battery("PROBE")     == false);	// "OB" in the middle
    CHECK(ups_on_battery("OL PROBE")  == false);	// and beside a real flag
    CHECK(ups_on_battery("FSD OBX")   == false);	// "OB" as a prefix
    CHECK(ups_on_battery("ALBUM")     == false);	// "LB" in the middle
    CHECK(ups_on_battery("ALARM BLB") == false);	// "LB" at the end

    // Nothing said is not "on mains"; the caller tells those apart by
    // whether it got a reading at all.
    CHECK(ups_on_battery("")   == false);
    CHECK(ups_on_battery(NULL) == false);

    //== ups_runtime: what the UPS reports =============================

    // battery.runtime wins whenever it is there, on battery or not,
    // and is never flagged as an estimate.
    {
	struct ups_reading r = BLANK;
	r.status = "OB"; r.runtime = 900;
	CHECK(NEAR(ups_runtime(&r, &est), 900));
	CHECK(est == false);
    }
    {
	struct ups_reading r = BLANK;
	r.status = "OL"; r.runtime = 7200;
	CHECK(NEAR(ups_runtime(&r, &est), 7200));
	CHECK(est == false);
    }
    // Zero is a reported value, not a missing one: a UPS about to drop.
    {
	struct ups_reading r = BLANK;
	r.status = "OB LB"; r.runtime = 0;
	CHECK(NEAR(ups_runtime(&r, &est), 0));
	CHECK(est == false);
    }

    //== ups_runtime: the estimate =====================================

    // Half a 0.6 Ah pack at 0.3 A: one hour.
    {
	struct ups_reading r = BLANK;
	r.status = "OB"; r.charge = 50; r.capacity = 0.6; r.current = -0.3;
	CHECK(NEAR(ups_runtime(&r, &est), 3600));
	CHECK(est == true);
    }
    // Full pack, twice the draw: half as long.
    {
	struct ups_reading r = BLANK;
	r.status = "OB"; r.charge = 100; r.capacity = 0.6; r.current = -1.2;
	CHECK(NEAR(ups_runtime(&r, &est), 1800));
	CHECK(est == true);
    }
    // A flat battery has no time left, and that is a reading, not a gap.
    {
	struct ups_reading r = BLANK;
	r.status = "OB LB"; r.charge = 0; r.capacity = 0.6; r.current = -0.3;
	CHECK(NEAR(ups_runtime(&r, &est), 0));
	CHECK(est == true);
    }

    //== ups_runtime: when it must refuse to answer ====================

    // On mains. A charging pack has no meaningful remaining time, even
    // with every figure needed to compute one present -- this is the
    // live state on moses (HB OL CHRG, charge 97.6, current +0.013).
    {
	struct ups_reading r = BLANK;
	r.status = "HB OL CHRG";
	r.charge = 97.6; r.capacity = 0.6; r.current = 0.013;
	CHECK(ups_runtime(&r, &est) < 0);
	CHECK(est == false);
    }
    // On battery but the current has no sign: a driver reporting a bare
    // magnitude gets no answer rather than a guess.
    {
	struct ups_reading r = BLANK;
	r.status = "OB"; r.charge = 50; r.capacity = 0.6; r.current = 0.3;
	CHECK(ups_runtime(&r, &est) < 0);
    }
    // No current reported at all: NaN must fail the test, not pass it.
    {
	struct ups_reading r = BLANK;
	r.status = "OB"; r.charge = 50; r.capacity = 0.6;
	CHECK(ups_runtime(&r, &est) < 0);
	CHECK(est == false);
    }
    // No capacity, and no charge: each on its own is enough to refuse.
    {
	struct ups_reading r = BLANK;
	r.status = "OB"; r.charge = 50; r.current = -0.3;
	CHECK(ups_runtime(&r, &est) < 0);
    }
    {
	struct ups_reading r = BLANK;
	r.status = "OB"; r.capacity = 0.6; r.current = -0.3;
	CHECK(ups_runtime(&r, &est) < 0);
    }
    // A zero capacity would be a division by zero, not an infinity.
    {
	struct ups_reading r = BLANK;
	r.status = "OB"; r.charge = 50; r.capacity = 0; r.current = -0.3;
	CHECK(ups_runtime(&r, &est) < 0);
    }
    // Nothing at all.
    {
	struct ups_reading r = BLANK;
	CHECK(ups_runtime(&r, &est) < 0);
	CHECK(est == false);
    }

    printf("%s: %d checks, %d failures\n",
	   (failures == 0) ? "PASS" : "FAIL", checks, failures);
    return (failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
