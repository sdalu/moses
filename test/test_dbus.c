/*
 * Integration test for the system bus path -- a daemon's sink
 * (src/dbus_sink.c) and moses_display's source (src/display/source-dbus.c)
 * talking through a real dbus-daemon, in one process.
 *
 * It exercises exactly the things the datagram socket this replaced
 * could not do, in the order they matter on the machine:
 *
 *   1. A display that starts late gets what was already said. The
 *      producer emits before the consumer exists; the consumer then
 *      starts and must find the reading, stamped with the time the
 *      line says it was taken, and the producer marked online.
 *   2. What is said afterwards arrives as it is said.
 *   3. A producer that leaves the bus is seen to leave: the mark goes
 *      offline without anyone timing anything out.
 *   4. A producer that comes back is picked up again, cache and all,
 *      with nothing on the consumer's side re-done.
 *   5. A producer never seen is never claimed to be gone.
 *   6. The valve can be commanded over the bus, and only where there
 *      is a valve: SetState on the breaker is applied and comes back
 *      as a reading, garbage is refused before any handler sees it, a
 *      failure reaches the caller, and the other daemons answer
 *      UnknownMethod. (Who may call at all is the bus's policy, which
 *      a session bus does not enforce; see dbus/moses.conf.)
 *
 * Run it under a private bus:
 *
 *     dbus-run-session -- ./bin/test_dbus
 *
 * which is what ctest does. dbus-run-session sets
 * DBUS_SESSION_BUS_ADDRESS; the code under test asks for the system
 * bus, and libdbus lets DBUS_SYSTEM_BUS_ADDRESS say where that is, so
 * the first thing here is to point the one at the other. Without either
 * variable there is no bus to test against, and the test says so and
 * skips (exit 77, which ctest is told means skipped) rather than
 * failing on a machine that simply has no dbus-daemon.
 *
 * Everything the bus does is asynchronous, so every expectation is
 * waited for, with a deadline generous enough for a loaded CI runner
 * and short enough that a failure is not a long wait.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "breaker_state.h"
#include "dbus_sink.h"
#include "model.h"
#include "source.h"

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

#define LIT(s)		(s), (sizeof(s) - 1)
#define NEAR(a, b)	(fabs((a) - (b)) < 1e-9)

/* How long an expectation may take to come true. The bus round trips
 * are milliseconds; the source's own tick is 200 ms; the rest is
 * margin for a slow machine. */
#define DEADLINE_MS	5000


/*
 * Wait until `pred` holds of the model, or the deadline passes.
 */
static bool
wait_for(bool (*pred)(const struct model *), int deadline_ms)
{
    for (int waited = 0 ; waited < deadline_ms ; waited += 20) {
	struct model m;
	model_get(&m);
	if (pred(&m))
	    return true;
	usleep(20 * 1000);
    }
    return false;
}


/* One line, as put_data() would build it, stamped `at` seconds. */
static void
emit(const char *measurement, const char *fields, time_t at)
{
    char line[256];
    int  n = snprintf(line, sizeof(line), "%s %s %lld000000000",
		      measurement, fields, (long long)at);
    dbus_sink_put(line, (size_t)n);
}


//== A caller, and the breaker's half of SetState ======================

/*
 * Send SetState to `dest` as any client would, and say how it went:
 * NULL for a plain reply, else the error's name, copied into `errname`.
 */
static const char *
call_set_state(const char *dest, const char *path, const char *state,
	       char *errname, size_t errlen)
{
    DBusError err;
    dbus_error_init(&err);

    DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    if (c == NULL) {
	snprintf(errname, errlen, "no bus: %s", err.message);
	dbus_error_free(&err);
	return errname;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);

    DBusMessage *m = dbus_message_new_method_call(dest, path, "moses.Actuator",
						  "SetState");
    dbus_message_append_args(m, DBUS_TYPE_STRING, &state, DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(c, m, 2000,
								   &err);
    dbus_message_unref(m);

    const char *result = NULL;
    if (reply == NULL) {
	snprintf(errname, errlen, "%s", err.name);
	dbus_error_free(&err);
	result = errname;
    } else {
	dbus_message_unref(reply);
    }
    dbus_connection_close(c);
    dbus_connection_unref(c);
    return result;
}


/* What moses_breaker does with a command, minus the relay: the same
 * parser, then the state emitted as a line -- which is what the real
 * breaker_set_state() does through PUT_DATA(). "0" is made to fail, to
 * see the failure reach the caller. */
static char set_state_seen[32];

static int
on_set_state(const char *state)
{
    int s = breaker_parse_state(state, (int)strlen(state));
    if (s < 0)
	return -1;
    snprintf(set_state_seen, sizeof(set_state_seen), "%s", state);
    if (s == 0)
	return -2;
    emit("breaker", "state=1", time(NULL));
    return 0;
}


//== Predicates ========================================================

static bool
index_known_and_online(const struct model *m)
{
    return m->index.known && NEAR(m->index.litres, 213044.0) &&
	   (m->avail[MODEL_WATERMETER] == MODEL_AVAIL_ONLINE);
}

static bool
pulse_is_three(const struct model *m)
{
    return m->pulse.known && (m->pulse.count == 3);
}

static bool
watermeter_offline(const struct model *m)
{
    return m->avail[MODEL_WATERMETER] == MODEL_AVAIL_OFFLINE;
}

static bool
index_updated_and_online(const struct model *m)
{
    return m->index.known && NEAR(m->index.litres, 213050.0) &&
	   (m->avail[MODEL_WATERMETER] == MODEL_AVAIL_ONLINE);
}

static bool
temperature_known(const struct model *m)
{
    return m->temperature.known && NEAR(m->temperature.celsius, 21.42);
}

static bool
valve_closed(const struct model *m)
{
    return m->valve.known && m->valve.closed &&
	   (m->avail[MODEL_BREAKER] == MODEL_AVAIL_ONLINE);
}


//== The test ==========================================================

int
main(void)
{
    /* The bus. See the top of the file. */
    if (getenv("DBUS_SYSTEM_BUS_ADDRESS") == NULL) {
	const char *session = getenv("DBUS_SESSION_BUS_ADDRESS");
	if (session == NULL) {
	    fprintf(stderr, "SKIP: no bus to test against "
		    "(run under dbus-run-session)\n");
	    return 77;
	}
	setenv("DBUS_SYSTEM_BUS_ADDRESS", session, 1);
    }

    model_init();

    time_t now = time(NULL);

    /* 1. The producer speaks first, to nobody. */
    dbus_sink_start("watermeter");
    emit("watermeter", "index=213044.000", now - 30);

    /* ... and the consumer arrives afterwards. It must find the index
     * by asking, not by waiting for the next report, see the producer
     * as online, and stamp the reading with the line's own time. */
    enum source_dbus_name name;
    CHECK(source_dbus_start(&name) == 0);
    /* A session bus lets anyone own anything, so the name is had; what
     * the system bus's policy decides is dbus/moses.conf's to test. */
    CHECK(name == SOURCE_DBUS_NAME_OWNED);
    CHECK(wait_for(index_known_and_online, DEADLINE_MS));
    {
	struct model m;
	model_get(&m);
	/* Within a couple of seconds of now - 30: the line's time, not
	 * the arrival, and not a 1970 from a clock that was not set. */
	CHECK(labs((long)(m.index.at - (now - 30))) <= 2);
    }

    /* 2. Live: a line emitted now arrives on its own. */
    emit("watermeter", "pulse=3", time(NULL));
    CHECK(wait_for(pulse_is_three, DEADLINE_MS));

    /* A failure is understood and changes no figure; garbage is
     * refused. Neither must upset anything. */
    emit("watermeter", "failure=\"read\"", time(NULL));
    dbus_sink_put(LIT("this is not a line"));
    usleep(300 * 1000);
    {
	struct model m;
	model_get(&m);
	CHECK(NEAR(m.index.litres, 213044.0));
	CHECK(m.pulse.count == 3);
    }

    /* 3. The producer leaves: the bus says so, at once. */
    dbus_sink_stop();
    CHECK(wait_for(watermeter_offline, DEADLINE_MS));

    /* 4. It comes back, with a new reading, and the consumer -- which
     * did nothing about any of this -- has it and the mark. A line
     * stamped in 1970 (a Pi before NTP) is stamped on arrival instead. */
    dbus_sink_start("watermeter");
    emit("watermeter", "index=213050.000", 86400);
    CHECK(wait_for(index_updated_and_online, DEADLINE_MS));
    {
	struct model m;
	model_get(&m);
	CHECK(labs((long)(m.index.at - time(NULL))) <= 2);
    }

    /* A second producer, with a line the panel reads one field of. */
    {
	struct model m;
	model_get(&m);
	CHECK(m.avail[MODEL_SENSORS] == MODEL_AVAIL_UNKNOWN);
    }
    /* One sink per process is the daemons' shape, so the sensors are
     * played by the same sink under another name. */
    dbus_sink_stop();
    dbus_sink_start("sensors");
    emit("environment", "temperature=21.42,pressure=102134,humidity=45.30",
	 time(NULL));
    CHECK(wait_for(temperature_known, DEADLINE_MS));
    {
	struct model m;
	model_get(&m);
	CHECK(m.avail[MODEL_SENSORS] == MODEL_AVAIL_ONLINE);
	/* The watermeter is gone again, and was seen to go. */
	CHECK(m.avail[MODEL_WATERMETER] == MODEL_AVAIL_OFFLINE);
    }

    /* 5. The breaker was never on this bus. Nothing may say it is gone. */
    {
	struct model m;
	model_get(&m);
	CHECK(m.avail[MODEL_BREAKER] == MODEL_AVAIL_UNKNOWN);
	CHECK(! m.valve.known);
    }

    /* 6. SetState. Not on the sensors: no handler, so libdbus's own
     * UnknownMethod comes back and nothing here was called -- or, on a
     * bus enforcing dbus/moses.conf, AccessDenied from dbus-daemon
     * before the call goes anywhere, which is the same refusal one
     * layer earlier. */
    {
	char errname[128];
	const char *e = call_set_state("moses.sensors", "/moses/sensors",
				       "1", errname, sizeof(errname));
	CHECK((e != NULL) &&
	      ((strcmp(e, DBUS_ERROR_UNKNOWN_METHOD) == 0) ||
	       (strcmp(e, DBUS_ERROR_ACCESS_DENIED) == 0)));
	CHECK(set_state_seen[0] == '\0');
    }

    /* On the breaker: a state is applied and reported as a line, so
     * the display sees the valve close; garbage is InvalidArgs and
     * reaches no handler; a state that cannot be set is Failed. */
    dbus_sink_stop();
    dbus_sink_on_set_state(on_set_state);
    dbus_sink_start("breaker");
    {
	char errname[128];

	/* Wait for the name: the sink connects on its own time. */
	const char *e = NULL;
	for (int i = 0 ; i < 50 ; i++) {
	    e = call_set_state("moses.breaker", "/moses/breaker", "1",
			       errname, sizeof(errname));
	    if ((e == NULL) || (strcmp(e, DBUS_ERROR_SERVICE_UNKNOWN) != 0))
		break;
	    usleep(100 * 1000);
	}
	CHECK(e == NULL);
	CHECK(strcmp(set_state_seen, "1") == 0);
	CHECK(wait_for(valve_closed, DEADLINE_MS));

	set_state_seen[0] = '\0';
	e = call_set_state("moses.breaker", "/moses/breaker", "sideways",
			   errname, sizeof(errname));
	CHECK((e != NULL) && (strcmp(e, DBUS_ERROR_INVALID_ARGS) == 0));
	CHECK(set_state_seen[0] == '\0');

	e = call_set_state("moses.breaker", "/moses/breaker", "0",
			   errname, sizeof(errname));
	CHECK((e != NULL) && (strcmp(e, DBUS_ERROR_FAILED) == 0));
	CHECK(strcmp(set_state_seen, "0") == 0);
    }

    source_dbus_stop();
    dbus_sink_stop();

    printf("%s: %d checks, %d failures\n",
	   (failures == 0) ? "PASS" : "FAIL", checks, failures);
    return (failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
