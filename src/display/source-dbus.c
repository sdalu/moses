/*
 * moses_display -- the system bus source
 *
 * The consuming half of WITH_DBUS. Each daemon on this machine owns a
 * name on the system bus and serves the last line of each kind it
 * emitted (src/dbus_sink.h); this watches those names and turns what
 * they say into the model.
 *
 * Why it is worth having, when MQTT already carries all of this: the
 * broker is not on this machine. A reading taken here otherwise travels
 * over the network to another host and back again to reach a panel ten
 * centimetres away, and when that network is down the panel goes blank
 * while every daemon behind it is working perfectly. That is precisely
 * the moment somebody walks up to it. This path has neither hop and no
 * dependency off the box.
 *
 * Unlike the datagram socket this replaces, it has both of the things
 * the socket could not carry:
 *
 *   Retention. A daemon keeps the last line of each kind and serves it
 *   as a property, so a display started between reports reads the last
 *   index and the last valve state at once rather than showing dashes
 *   until the next one. Every line carries the time it was taken, and
 *   that is what the model is stamped with, so a value read back this
 *   way ages from when it was really taken.
 *
 *   Liveness. The bus emits NameOwnerChanged the moment a name's owner
 *   drops off it -- exits, crashes, or is killed -- which is a last will
 *   that arrives in milliseconds rather than after a keepalive lapses.
 *   A daemon that comes back reclaims its name and the same signal says
 *   so; nothing here has to be re-subscribed.
 *
 * What it will not do is claim a daemon is gone because its name was
 * never seen. A daemon built without the bus, or one whose name the
 * bus's policy refused, is alive and merely silent here, and the
 * availability marks are MQTT's to set until a name is actually seen
 * to appear or to go. So: a name that is owned sets ONLINE, a name seen
 * to lose its owner sets OFFLINE, and a name with no owner at startup
 * sets nothing.
 *
 * Parsing lives in lineproto.c, with a test, because it decides what
 * figure reaches the panel. This file is the bus.
 */

#ifndef WITH_DBUS
#error "source-dbus.c is only built under WITH_DBUS"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "common.h"

#include "lineproto.h"
#include "model.h"
#include "source.h"


#define SRC_IFACE		"moses.Readings"
#define SRC_NAMESPACE		"moses"

/* How long the bus thread waits inside the bus before looking at the
 * fetches queued by the filter and at whether it was asked to stop. */
#define SRC_TICK_MS		200

/* Seconds between attempts to reach a bus that has gone. */
#define SRC_RETRY_S		5

/* How long a daemon gets to answer for its Lines. It answers out of
 * memory, on the loopback; a second is already generous. */
#define SRC_FETCH_MS		1000

/* A line's own timestamp is used as the model's when it is plausible:
 * not in the future, and not older than this. The daemons are on this
 * same machine, so the clock is the same clock and the only way to be
 * outside the window is a clock that was not set yet when the reading
 * was taken -- a Pi with no RTC stamps its first readings in 1970. Such
 * a line is stamped with its arrival instead, as the socket did. */
#define SRC_LINE_AGE_MAX	(24 * 60 * 60)	/* seconds		*/


/* The daemons watched: name, object, and which mark is theirs. */
static const struct daemon {
    const char          *name;
    const char          *path;
    enum model_producer  who;
} daemons[] = {
    { "moses.watermeter", "/moses/watermeter", MODEL_WATERMETER },
    { "moses.breaker",    "/moses/breaker",    MODEL_BREAKER    },
    { "moses.sensors",    "/moses/sensors",    MODEL_SENSORS    },
};
#define DAEMON_COUNT	(sizeof(daemons) / sizeof(daemons[0]))

static struct {
    pthread_t     thread;
    volatile bool stopping;
    bool          started;

    /* Set by the filter, acted on by the loop: a daemon whose Lines
     * are to be fetched. The filter runs inside dispatch, and a
     * blocking call from there is legal but not worth the reentrancy;
     * the loop makes it a moment later, outside. */
    volatile bool fetch[DAEMON_COUNT];
} src;


//== Applying a line ===================================================

/*
 * The model's timestamp for a line: its own when plausible, else now.
 */
static time_t
stamp(time_t line_at)
{
    time_t now = time(NULL);

    if ((line_at > 0) && (line_at <= now) && (now - line_at < SRC_LINE_AGE_MAX))
	return line_at;
    return now;
}


/*
 * Everything one line had to say.
 */
static void
apply(const char *line)
{
    struct lineproto_reading r[LINEPROTO_READINGS_MAX];
    bool   ok = false;

    size_t n = lineproto_parse(line, strlen(line), r,
			       LINEPROTO_READINGS_MAX, &ok);
    if (! ok) {
	/* Logged, not counted: a producer that has gone wrong is worth
	 * seeing, and a display is not the place to keep statistics. */
	LOG("line not understood: %s", line);
	return;
    }

    for (size_t i = 0 ; i < n ; i++) {
	time_t at = stamp(r[i].at);

	switch (r[i].kind) {
	case LINEPROTO_INDEX:
	    model_set_index_at(r[i].litres, at);
	    break;
	case LINEPROTO_PULSE:
	    model_set_pulse_at(r[i].count, at);
	    break;
	case LINEPROTO_TEMPERATURE:
	    model_set_temperature_at(r[i].celsius, at);
	    break;
	case LINEPROTO_VALVE:
	    model_set_valve_at(r[i].closed, at);
	    break;
	case LINEPROTO_FAILURE:
	    /* The model has nowhere to put this: a reading that failed
	     * leaves the last good one on the screen, ageing, which is
	     * what a dash-when-stale already says. Worth logging. */
	    LOG("%s reports a failure: %s", r[i].measurement, r[i].failure);
	    break;
	case LINEPROTO_IGNORED:
	    break;
	}
    }
}


static const struct daemon *
daemon_by_name(const char *name)
{
    for (size_t i = 0 ; i < DAEMON_COUNT ; i++) {
	if (strcmp(daemons[i].name, name) == 0)
	    return &daemons[i];
    }
    return NULL;
}


static const struct daemon *
daemon_by_path(const char *path)
{
    for (size_t i = 0 ; i < DAEMON_COUNT ; i++) {
	if (strcmp(daemons[i].path, path) == 0)
	    return &daemons[i];
    }
    return NULL;
}


//== Fetching ==========================================================

/*
 * Ask one daemon for its Lines, and apply them.
 *
 * Blocking, on the bus thread, for at most SRC_FETCH_MS. A name with
 * no owner is the ordinary answer for a daemon that is not running or
 * not on the bus, and says nothing about the mark (see the top of the
 * file); anything else that goes wrong is logged.
 */
static void
fetch(DBusConnection *c, const struct daemon *d)
{
    const char *iface = SRC_IFACE, *prop = "Lines";
    DBusError   err;

    dbus_error_init(&err);

    DBusMessage *m = dbus_message_new_method_call(d->name, d->path,
						  DBUS_INTERFACE_PROPERTIES,
						  "Get");
    if ((m == NULL) ||
	! dbus_message_append_args(m, DBUS_TYPE_STRING, &iface,
				   DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
	if (m != NULL)
	    dbus_message_unref(m);
	return;
    }

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(c, m,
							SRC_FETCH_MS, &err);
    dbus_message_unref(m);

    if (reply == NULL) {
	if (! dbus_error_has_name(&err, DBUS_ERROR_SERVICE_UNKNOWN) &&
	    ! dbus_error_has_name(&err, DBUS_ERROR_NAME_HAS_NO_OWNER))
	    LOG("D-Bus: %s would not say: %s", d->name, err.message);
	dbus_error_free(&err);
	return;
    }

    /* v(as): a variant holding an array of strings. Checked at every
     * level rather than assumed, because the answer comes from another
     * process and a wrong shape must not be walked. */
    DBusMessageIter it, var, arr;
    size_t          applied = 0;

    if (dbus_message_iter_init(reply, &it) &&
	(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT)) {
	dbus_message_iter_recurse(&it, &var);
	if ((dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) &&
	    (dbus_message_iter_get_element_type(&var) == DBUS_TYPE_STRING)) {
	    dbus_message_iter_recurse(&var, &arr);
	    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
		const char *line = NULL;
		dbus_message_iter_get_basic(&arr, &line);
		if (line != NULL) {
		    apply(line);
		    applied++;
		}
		dbus_message_iter_next(&arr);
	    }
	}
    }
    dbus_message_unref(reply);

    /* It answered, so it is there. */
    model_set_avail(d->who, true);
    LOG("D-Bus: %s answered with %zu line(s)", d->name, applied);
    (void)applied;			/* only logged, and LOG may be nothing */
}


//== Signals ===========================================================

/*
 * Every message the connection receives. Runs on the bus thread.
 */
static DBusHandlerResult
on_signal(DBusConnection *c, DBusMessage *msg, void *data)
{
    (void)c;
    (void)data;

    DBusError err;
    dbus_error_init(&err);

    if (dbus_message_is_signal(msg, SRC_IFACE, "Line")) {
	const char *path = dbus_message_get_path(msg);
	const char *line = NULL;

	if ((path != NULL) &&
	    dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &line,
				  DBUS_TYPE_INVALID)) {
	    const struct daemon *d = daemon_by_path(path);
	    if (d != NULL) {
		apply(line);
		/* A line is proof of life, whatever the bus said. */
		model_set_avail(d->who, true);
	    }
	}
	dbus_error_free(&err);
	return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
	const char *name = NULL, *old = NULL, *new = NULL;

	if (dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &name,
				  DBUS_TYPE_STRING, &old,
				  DBUS_TYPE_STRING, &new, DBUS_TYPE_INVALID)) {
	    const struct daemon *d = daemon_by_name(name);
	    if (d != NULL) {
		if (new[0] != '\0') {
		    LOG("D-Bus: %s is on the bus", d->name);
		    model_set_avail(d->who, true);
		    src.fetch[d - daemons] = true;
		} else if (old[0] != '\0') {
		    LOG("D-Bus: %s has left the bus", d->name);
		    model_set_avail(d->who, false);
		}
	    }
	}
	dbus_error_free(&err);
	return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


//== The bus thread ====================================================

/*
 * Reach the bus and ask for what this wants to hear.
 *
 * @return the connection, or NULL with the reason logged
 */
static DBusConnection *
src_connect(void)
{
    DBusError err;
    dbus_error_init(&err);

    DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    if (c == NULL) {
	LOG("D-Bus: cannot reach the system bus: %s", err.message);
	dbus_error_free(&err);
	return NULL;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);

    /* The lines, from anyone speaking the interface, and the names
     * under moses. coming and going. Both matched at the bus, so
     * nothing else on it is delivered here at all. */
    static const char *const rules[] = {
	"type='signal',interface='" SRC_IFACE "',member='Line'",
	"type='signal',sender='" DBUS_SERVICE_DBUS "',"
	    "interface='" DBUS_INTERFACE_DBUS "',member='NameOwnerChanged',"
	    "arg0namespace='" SRC_NAMESPACE "'",
    };
    for (size_t i = 0 ; i < sizeof(rules) / sizeof(rules[0]) ; i++) {
	dbus_bus_add_match(c, rules[i], &err);
	if (dbus_error_is_set(&err)) {
	    LOG("D-Bus: cannot match %s: %s", rules[i], err.message);
	    dbus_error_free(&err);
	    dbus_connection_close(c);
	    dbus_connection_unref(c);
	    return NULL;
	}
    }

    if (! dbus_connection_add_filter(c, on_signal, NULL, NULL)) {
	LOG("D-Bus: cannot add the filter");
	dbus_connection_close(c);
	dbus_connection_unref(c);
	return NULL;
    }

    return c;
}


static void
src_backoff(void)
{
    for (int i = 0 ; (i < SRC_RETRY_S * 10) && ! src.stopping ; i++)
	usleep(100 * 1000);
}


static void *
src_thread(void *arg)
{
    DBusConnection *c = arg;		/* the first one, opened by start() */

    while (! src.stopping) {
	if (c == NULL) {
	    c = src_connect();
	    if (c == NULL) {
		src_backoff();
		continue;
	    }
	    LOG("D-Bus: back on the system bus");
	}

	/* Everything that is there now. Signals are matched before this
	 * is asked, so a line emitted meanwhile is not lost; at worst it
	 * is applied twice. */
	for (size_t i = 0 ; i < DAEMON_COUNT ; i++)
	    src.fetch[i] = true;

	while (! src.stopping &&
	       dbus_connection_read_write_dispatch(c, SRC_TICK_MS)) {
	    for (size_t i = 0 ; i < DAEMON_COUNT ; i++) {
		if (src.fetch[i]) {
		    src.fetch[i] = false;
		    fetch(c, &daemons[i]);
		}
	    }
	}

	dbus_connection_close(c);
	dbus_connection_unref(c);
	c = NULL;

	if (! src.stopping) {
	    /* The bus itself went, and with it every name on it. The
	     * marks are left as they were: the daemons did not die, the
	     * thing that would have told us did. */
	    LOG("D-Bus: lost the system bus, retrying");
	    src_backoff();
	}
    }
    return NULL;
}


//== Interface =========================================================

int
source_dbus_start(void)
{
    if (src.started)
	return 0;

    if (! dbus_threads_init_default()) {
	LOG("D-Bus: cannot initialise threading");
	errno = ENOMEM;
	return -1;
    }

    /* The first connection is made here rather than on the thread, so
     * that a bus that is not there at all is a failure main() sees. A
     * bus that goes away later is the thread's to get back. */
    DBusConnection *c = src_connect();
    if (c == NULL) {
	errno = ECONNREFUSED;
	return -1;
    }

    src.stopping = false;
    memset((void *)src.fetch, 0, sizeof(src.fetch));

    int rc = pthread_create(&src.thread, NULL, src_thread, c);
    if (rc != 0) {
	errno = rc;
	LOG_ERRNO("failed to start the bus thread");
	dbus_connection_close(c);
	dbus_connection_unref(c);
	return -1;
    }
    src.started = true;

    LOG("D-Bus                : system bus, " SRC_NAMESPACE ".*");
    return 0;
}


void
source_dbus_stop(void)
{
    if (! src.started)
	return;

    src.stopping = true;
    pthread_join(src.thread, NULL);
    src.started = false;
}
