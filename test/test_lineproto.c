/*
 * Unit tests for the line-protocol reader -- what moses_display makes of
 * what arrives on the local datagram socket.
 *
 * The lines here are not invented: they are what src/common.c's
 * put_data() and put_fail() build from the format strings in
 * src/watermeter.c, src/sensors.c and src/breaker.c. That is the point
 * of the first group of tests. The two sides agree on the measurement
 * names and the field names by nothing but four files using the same
 * string literals, so the wire as it is actually emitted is written out
 * here and asserted on; if a producer's format string changes, this is
 * what is supposed to go red.
 *
 * The rest is the refusals. A datagram socket is not authenticated and
 * not framed by anything but the datagram, so anything on this machine
 * can write to it, and a half-read line is how a wrong figure reaches
 * the panel. Every one of these checks that a malformed line yields
 * nothing at all rather than the part of it that happened to parse.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lineproto.h"

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

/* Lines arrive as a buffer and a length, so that is how they are passed:
 * LIT() is a string literal without its NUL. */
#define LIT(s)		(s), (sizeof(s) - 1)

#define NEAR(a, b)	(fabs((a) - (b)) < 1e-9)

/* One parse, into a fresh set of readings. */
#define PARSE(...)							\
    struct lineproto_reading r[LINEPROTO_READINGS_MAX];			\
    bool   ok = false;							\
    size_t n  = lineproto_parse(__VA_ARGS__, r,				\
				LINEPROTO_READINGS_MAX, &ok)


/*
 * The wire, exactly as the daemons emit it.
 *
 * Each line below is put_data()'s "%s %s %lld%09ld" filled in with the
 * format string from the producer named in the comment.
 */
static void
test_the_real_wire(void)
{
    /* src/watermeter.c: PUT_DATA("watermeter", "index=%0.3f", value) */
    {
	PARSE(LIT("watermeter index=213044.000 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_INDEX);
	CHECK(NEAR(r[0].litres, 213044.0));
	CHECK(strcmp(r[0].measurement, "watermeter") == 0);
    }

    /* src/watermeter.c: PUT_DATA("watermeter", "pulse=%d", pulse) */
    {
	PARSE(LIT("watermeter pulse=3 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_PULSE);
	CHECK(r[0].count == 3);
    }

    /* A pulse report that counted nothing still arrives, and still says
     * the meter was read -- the dashboard latches on a non-zero one. */
    {
	PARSE(LIT("watermeter pulse=0 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_PULSE);
	CHECK(r[0].count == 0);
    }

    /* src/sensors.c: three fields, of which one is on the panel. The
     * other two are understood and discarded, which is why n is 1 and
     * not 3 -- and why the line is still `ok`. */
    {
	PARSE(LIT("environment temperature=21.42,pressure=102134,"
		  "humidity=45.30 1790489588441443042"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_TEMPERATURE);
	CHECK(NEAR(r[0].celsius, 21.42));
	CHECK(strcmp(r[0].measurement, "environment") == 0);
    }

    /* src/breaker.c: PUT_DATA(NICKNAME, "state=%d", state), NICKNAME
     * being "breaker". 1 is the relay energised: the water is shut. */
    {
	PARSE(LIT("breaker state=1 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_VALVE);
	CHECK(r[0].closed == true);
    }
    {
	PARSE(LIT("breaker state=0 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_VALVE);
	CHECK(r[0].closed == false);
    }

    /* src/common.c: put_fail(), whose quotes are required by line
     * protocol. The three failures the daemons send. */
    {
	PARSE(LIT("watermeter failure=\"read\" 1790489588441449592"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_FAILURE);
	CHECK(strcmp(r[0].failure, "read") == 0);
	CHECK(strcmp(r[0].measurement, "watermeter") == 0);
    }
    {
	PARSE(LIT("watermeter failure=\"pulse\" 1790489588441449592"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_FAILURE);
	CHECK(strcmp(r[0].failure, "pulse") == 0);
    }
    {
	PARSE(LIT("breaker failure=\"set-state\" 1790489588441449592"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_FAILURE);
	CHECK(strcmp(r[0].failure, "set-state") == 0);
    }
    {
	PARSE(LIT("environment failure=\"read\" 1790489588441449592"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_FAILURE);
    }
}


/*
 * The newline. The socket sink sends the line without one and the stdout
 * sink writes it with one, and the same reader has to take either --
 * someone will point this at a log file or a pipe.
 */
static void
test_terminators(void)
{
    {
	PARSE(LIT("watermeter index=1.5 1790489588441408829\n"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(NEAR(r[0].litres, 1.5));
    }
    {
	PARSE(LIT("watermeter index=1.5 1790489588441408829\r\n"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(NEAR(r[0].litres, 1.5));
    }
    /* Nothing but a terminator is not a line */
    {
	PARSE(LIT("\n"));
	CHECK(! ok);
	CHECK(n == 0);
    }
}


/*
 * The timestamp, which is skipped rather than used -- but checked to be
 * a number, and that check is load-bearing. Fields are cut at the first
 * space, so anything that puts a space inside them would leave the rest
 * of the line looking like a timestamp; refusing a non-numeric one is
 * what stops half a line being read as all of it.
 */
static void
test_timestamp(void)
{
    /* Read to the second, and handed back on every reading of the line */
    {
	PARSE(LIT("environment temperature=21.42,pressure=102134,"
		  "humidity=45.30 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].at == (time_t)1790489588);
    }

    /* Absent is allowed, and reads as 0: "no usable timestamp" */
    {
	PARSE(LIT("watermeter index=99"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(NEAR(r[0].litres, 99));
	CHECK(r[0].at == 0);
    }

    /* Digits, but too many to be a time: the line is read, the
     * timestamp is not. Twenty-one digits is past what strtoull()
     * holds, and a date from a saturated value is worse than none. */
    {
	PARSE(LIT("watermeter index=99 179048958844140882912"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].at == 0);
    }

    /* Not a number: the line is not the shape this reads */
    {
	PARSE(LIT("watermeter index=99 later"));
	CHECK(! ok);
	CHECK(n == 0);
    }
    {
	PARSE(LIT("watermeter index=99 17904895884414088299999x"));
	CHECK(! ok);
	CHECK(n == 0);
    }

    /* A space inside a quoted value: the reason the check above exists.
     * Without it this would read `failure="set` and call it a failure. */
    {
	PARSE(LIT("breaker failure=\"set state\" 1790489588441449592"));
	CHECK(! ok);
	CHECK(n == 0);
    }

    /* Two spaces, so the "timestamp" is empty */
    {
	PARSE(LIT("watermeter index=99  1790489588441408829"));
	CHECK(! ok);
	CHECK(n == 0);
    }
}


/*
 * Values that are well formed as text and must not become figures.
 * These go through payload_double() and payload_ulong(), which is the
 * point: a reading off the socket is accepted on the same terms as one
 * off the broker.
 */
static void
test_refused_values(void)
{
    /* NaN and infinity: strtod() takes them, the panel must not. An
     * index of NaN would be drawn as "nan L" and read as a measurement */
    {
	PARSE(LIT("watermeter index=nan 1790489588441408829"));
	CHECK(ok);			/* the line parsed */
	CHECK(n == 0);			/* the value did not */
    }
    {
	PARSE(LIT("watermeter index=inf 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }
    {
	PARSE(LIT("environment temperature=nan 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }

    /* A count is whole. "3.7" is not three, it is a broken producer */
    {
	PARSE(LIT("watermeter pulse=3.7 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }
    {
	PARSE(LIT("watermeter pulse=-1 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }

    /* The breaker's vocabulary is breaker_parse_state()'s, and 2 is not
     * in it -- the valve is shut or it is not */
    {
	PARSE(LIT("breaker state=2 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }
    {
	PARSE(LIT("breaker state=open 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }

    /* ...but the rest of that vocabulary is in it, because it is the
     * same parser moses_breaker reads its own commands with */
    {
	PARSE(LIT("breaker state=on 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_VALVE);
	CHECK(r[0].closed == true);
    }

    /* An unquoted failure is not one: put_fail() quotes it, and line
     * protocol requires that, so this did not come from put_fail() */
    {
	PARSE(LIT("watermeter failure=read 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }
    /* Nor is a half-quoted one. The line still passes for well formed:
     * the fields are cut at the space, so what is left is the digits of
     * a timestamp and the shape is exactly right. It is the field that
     * is refused, for having an opening quote and no closing one --
     * which is the half that matters, since the alternative is calling
     * `"read` the reason the meter failed. */
    {
	PARSE(LIT("watermeter failure=\"read 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }
}


/*
 * Lines that are not this protocol. All of them must yield nothing --
 * not the fields they managed to read first.
 */
static void
test_malformed(void)
{
    /* No fields at all */
    {
	PARSE(LIT("watermeter"));
	CHECK(! ok);
	CHECK(n == 0);
    }
    {
	PARSE(LIT("watermeter "));
	CHECK(! ok);
	CHECK(n == 0);
    }

    /* A field that is not key=value. The second is the one that matters:
     * `index=1` parses, and `garbage` after it must still sink the line
     * rather than leaving a figure behind from the first half. */
    {
	PARSE(LIT("watermeter garbage 1790489588441408829"));
	CHECK(! ok);
	CHECK(n == 0);
    }
    {
	PARSE(LIT("watermeter index=1,garbage 1790489588441408829"));
	CHECK(! ok);
	CHECK(n == 0);
    }
    /* An empty key */
    {
	PARSE(LIT("watermeter =5 1790489588441408829"));
	CHECK(! ok);
	CHECK(n == 0);
    }

    /* A tag set. Legal line protocol, not something these producers
     * emit, and its fields are in a different place than assumed */
    {
	PARSE(LIT("watermeter,host=moses index=1 1790489588441408829"));
	CHECK(! ok);
	CHECK(n == 0);
    }

    /* No measurement */
    {
	PARSE(LIT(" index=1 1790489588441408829"));
	CHECK(! ok);
	CHECK(n == 0);
    }

    /* Nothing at all, and a NUL in the middle -- the latter because
     * every comparison in the parser is a string function, and a line
     * that ends early would be read as a shorter one that parses */
    {
	PARSE(LIT(""));
	CHECK(! ok);
	CHECK(n == 0);
    }
    {
	PARSE("watermeter index=1\0 x", 21);
	CHECK(! ok);
	CHECK(n == 0);
    }
    {
	struct lineproto_reading r[LINEPROTO_READINGS_MAX];
	bool ok = false;
	CHECK(lineproto_parse(NULL, 10, r, LINEPROTO_READINGS_MAX, &ok) == 0);
	CHECK(! ok);
    }

    /* A measurement nobody here knows. Understood, and nothing to show:
     * another producer on the same socket is not an error */
    {
	PARSE(LIT("furnace temperature=60 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }
    /* A field this measurement does not have, for the same reason */
    {
	PARSE(LIT("watermeter litres=5 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 0);
    }
}


/*
 * More than one reading in a line, and more than there is room for.
 */
static void
test_several(void)
{
    /* Not a line any producer sends today, but the reader takes each
     * field on its own merits and two of these are on the panel */
    {
	PARSE(LIT("watermeter index=10.5,pulse=2 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 2);
	CHECK(r[0].kind == LINEPROTO_INDEX);
	CHECK(NEAR(r[0].litres, 10.5));
	CHECK(r[1].kind == LINEPROTO_PULSE);
	CHECK(r[1].count == 2);
    }

    /* Room for one: what fits is kept, and the line is still good --
     * dropping a reading for want of space must not discard the others */
    {
	struct lineproto_reading one[1];
	bool   ok = false;
	size_t n  = lineproto_parse(
	    LIT("watermeter index=10.5,pulse=2 1790489588441408829"),
	    one, 1, &ok);
	CHECK(ok);
	CHECK(n == 1);
	CHECK(one[0].kind == LINEPROTO_INDEX);
    }

    /* No room at all */
    {
	struct lineproto_reading none[1];
	bool ok = false;
	CHECK(lineproto_parse(LIT("watermeter index=1 1790489588441408829"),
			      none, 0, &ok) == 0);
	CHECK(! ok);
    }

    /* `ok` is optional: a caller that only wants the figures */
    {
	struct lineproto_reading r[LINEPROTO_READINGS_MAX];
	CHECK(lineproto_parse(LIT("watermeter index=7 1790489588441408829"),
			      r, LINEPROTO_READINGS_MAX, NULL) == 1);
	CHECK(NEAR(r[0].litres, 7));
    }
}


/*
 * The lengths. A datagram is whatever the sender chose to send.
 */
static void
test_lengths(void)
{
    /* Longer than any line put_data() can build */
    {
	char big[LINEPROTO_MAX + 64];
	memset(big, 'x', sizeof(big));
	struct lineproto_reading r[LINEPROTO_READINGS_MAX];
	bool ok = false;
	CHECK(lineproto_parse(big, sizeof(big), r,
			      LINEPROTO_READINGS_MAX, &ok) == 0);
	CHECK(! ok);
    }

    /* A failure longer than there is room to keep: truncated, not
     * refused, because it is only ever logged */
    {
	PARSE(LIT("watermeter failure=\"a-failure-name-far-longer-than-"
		  "anything-this-tree-emits\" 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_FAILURE);
	CHECK(strlen(r[0].failure) == LINEPROTO_NAME_MAX - 1);
    }

    /* An empty quoted failure: two quotes and nothing between them */
    {
	PARSE(LIT("watermeter failure=\"\" 1790489588441408829"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_FAILURE);
	CHECK(r[0].failure[0] == '\0');
    }
}


/*
 * The leak report: one reading made of several fields, exactly as
 * src/watermeter.c's leak_publish() writes it.
 */
static void
test_leak(void)
{
    {
	PARSE(LIT("watermeter leak=2,kind=\"flow\",since=1790483260,volume=134,"
		  "rate=5.60,source=\"index\" 1790489588441452110"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_LEAK);
	CHECK(r[0].leak.level == 2);
	CHECK(strcmp(r[0].leak.kind, "flow") == 0);
	CHECK(r[0].leak.since == 1790483260);
	CHECK(NEAR(r[0].leak.rate, 5.60));
	CHECK(r[0].at == 1790489588);
    }
    {
	/* All clear: what is published at start and after a leak ends */
	PARSE(LIT("watermeter leak=0,kind=\"none\",since=0,volume=0,rate=0.00,"
		  "source=\"pulse\" 1790489588441452110"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_LEAK);
	CHECK(r[0].leak.level == 0);
	CHECK(strcmp(r[0].leak.kind, "none") == 0);
    }
    {
	/* A level these producers never send is not a leak reading */
	PARSE(LIT("watermeter leak=7,kind=\"flow\" 1790489588441452110"));
	CHECK(ok);
	CHECK(n == 0);
    }
    {
	/* kind, since and rate mean something only after leak= */
	PARSE(LIT("watermeter index=5,kind=\"flow\",rate=3 1790489588441452110"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(r[0].kind == LINEPROTO_INDEX);
    }
    {
	/* A kind too long to be one of the four is left as "none" */
	PARSE(LIT("watermeter leak=1,kind=\"torrential\" 1790489588441452110"));
	CHECK(ok);
	CHECK(n == 1);
	CHECK(strcmp(r[0].leak.kind, "none") == 0);
    }
}


int
main(void)
{
    test_the_real_wire();
    test_terminators();
    test_timestamp();
    test_refused_values();
    test_malformed();
    test_several();
    test_lengths();
    test_leak();

    printf("%s: %d checks, %d failures\n",
	   (failures == 0) ? "PASS" : "FAIL", checks, failures);
    return (failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
