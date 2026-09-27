/*
 * dbus_sink.c -- each reading on the system bus
 *
 * See dbus_sink.h for the interface and for why it is shaped so. What
 * is here is the mechanics: a cache of the last line of each kind, a
 * thread that owns the connection, and the handler that answers for
 * the object.
 *
 * Two threads touch the connection, and libdbus is built for that once
 * dbus_threads_init_default() has been called: the bus thread blocks in
 * dbus_connection_read_write_dispatch(), which drops the connection's
 * own lock while it waits, and the caller of dbus_sink_put() queues a
 * signal with dbus_connection_send() from wherever it is. The mutex
 * here is not for libdbus; it is for the cache and for the connection
 * pointer, which the bus thread nulls before it closes a connection
 * that has dropped, so that a put in flight never sends on a closed
 * one.
 *
 * The bus thread wakes every SINK_TICK_MS rather than blocking for
 * ever, because a signal queued from another thread is written by the
 * next iteration of the loop and nothing wakes that loop up for it:
 * libdbus's wakeup hook belongs to a main loop this program does not
 * have. Two hundred milliseconds is nothing beside a reading interval
 * of a minute, and a bus thread that sleeps is one that costs nothing.
 */

#ifndef WITH_DBUS
#error "dbus_sink.c is only built under WITH_DBUS"
#endif

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "common.h"
#include "dbus_sink.h"


#define SINK_IFACE		"moses.Readings"
#define SINK_ACTUATOR_IFACE	"moses.Actuator"
#define SINK_NAME_PREFIX	"moses."
#define SINK_PATH_PREFIX	"/moses/"

/* Room for the longest line put_data() can build. */
#define SINK_LINE_MAX		768

/* A kind is "<measurement> <first field>", and the longest one this tree
 * emits is "environment temperature" at 23. */
#define SINK_KEY_MAX		64

/* Kinds kept. The most any one daemon emits is three -- the watermeter's
 * index, pulse and failure -- so this is room for every kind of a
 * daemon that has grown, not a limit anything approaches. */
#define SINK_LINES_MAX		8

/* Room for moses.<daemon> and /moses/<daemon>. */
#define SINK_NAME_MAX		64

/* How long the bus thread waits inside the bus before looking at the
 * outgoing queue and at whether it was asked to stop. */
#define SINK_TICK_MS		200

/* Seconds between attempts to reach a bus that was not there, or that
 * refused the name. Long enough not to be a spin, short enough that a
 * dbus-daemon restarting is caught before the next reading. */
#define SINK_RETRY_S		5

/* Outgoing bytes queued before further signals are dropped. A line is
 * under a hundred bytes and a consumer reads them at once, so a queue
 * this deep is a bus that has stopped draining, not a busy one. */
#define SINK_BACKLOG_MAX	(64 * 1024)


struct sink_line {
    char key[SINK_KEY_MAX];		/* "<measurement> <first field>"	*/
    char line[SINK_LINE_MAX];		/* the line, NUL-terminated	*/
};

static struct {
    pthread_mutex_t  mutex;
    DBusConnection  *conn;		/* NULL while the bus is out of reach */
    char             name[SINK_NAME_MAX];	/* moses.<daemon>	*/
    char             path[SINK_NAME_MAX];	/* /moses/<daemon>	*/
    struct sink_line lines[SINK_LINES_MAX];
    size_t           nlines;
    pthread_t        thread;
    bool             started;
    volatile bool    stopping;
    dbus_sink_set_state_t set_state;	/* NULL: no moses.Actuator here	*/
} sink = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .conn  = NULL,
};


/* What the object says about itself, in two halves: moses.Actuator is
 * listed only where a handler was given, so the watermeter and the
 * sensors introspect without a method they would refuse. The standard
 * interfaces are listed because introspection tools expect to see
 * them. */
static const char introspection_head[] =
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE
    "<node>\n"
    "  <interface name=\"" SINK_IFACE "\">\n"
    "    <property name=\"Lines\" type=\"as\" access=\"read\"/>\n"
    "    <signal name=\"Line\">\n"
    "      <arg name=\"line\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n";

static const char introspection_actuator[] =
    "  <interface name=\"" SINK_ACTUATOR_IFACE "\">\n"
    "    <method name=\"SetState\">\n"
    "      <arg name=\"state\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "  </interface>\n";

static const char introspection_tail[] =
    "  <interface name=\"" DBUS_INTERFACE_INTROSPECTABLE "\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"" DBUS_INTERFACE_PROPERTIES "\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "</node>\n";


//== The cache =========================================================

/*
 * The kind of a line: everything up to and including the first field's
 * name. "watermeter index=213044.000 1790..." is the kind "watermeter
 * index". A line with no '=' at all is its own kind, whole, which keeps
 * a malformed one from overwriting a good one.
 */
static void
line_key(const char *line, char *key, size_t keylen)
{
    const char *eq = strchr(line, '=');
    size_t      n  = (eq != NULL) ? (size_t)(eq - line) : strlen(line);

    if (n >= keylen)
	n = keylen - 1;
    memcpy(key, line, n);
    key[n] = '\0';
}


/* Keep `line` as the last of its kind. Under the mutex. */
static void
cache_put(const char *line)
{
    char key[SINK_KEY_MAX];
    line_key(line, key, sizeof(key));

    struct sink_line *slot = NULL;
    for (size_t i = 0 ; i < sink.nlines ; i++) {
	if (strcmp(sink.lines[i].key, key) == 0) {
	    slot = &sink.lines[i];
	    break;
	}
    }
    if (slot == NULL) {
	if (sink.nlines >= SINK_LINES_MAX)
	    return;			/* signalled, not retained */
	slot = &sink.lines[sink.nlines++];
	snprintf(slot->key, sizeof(slot->key), "%s", key);
    }
    snprintf(slot->line, sizeof(slot->line), "%s", line);
}


/* Append the cached lines as an `as`. Under the mutex. */
static bool
append_lines(DBusMessageIter *iter)
{
    DBusMessageIter arr;

    if (! dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					   DBUS_TYPE_STRING_AS_STRING, &arr))
	return false;

    for (size_t i = 0 ; i < sink.nlines ; i++) {
	const char *s = sink.lines[i].line;
	if (! dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &s)) {
	    dbus_message_iter_abandon_container(iter, &arr);
	    return false;
	}
    }
    return dbus_message_iter_close_container(iter, &arr);
}


/* The Lines property as a variant. Under the mutex. */
static bool
append_lines_variant(DBusMessageIter *iter)
{
    DBusMessageIter var;

    if (! dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
					   "as", &var))
	return false;
    if (! append_lines(&var)) {
	dbus_message_iter_abandon_container(iter, &var);
	return false;
    }
    return dbus_message_iter_close_container(iter, &var);
}


//== The object ========================================================

static void
send_reply(DBusConnection *c, DBusMessage *reply)
{
    if (reply == NULL)
	return;				/* out of memory: the caller times out */
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}


static DBusHandlerResult
on_introspect(DBusConnection *c, DBusMessage *msg)
{
    char xml[sizeof(introspection_head) + sizeof(introspection_actuator) +
	     sizeof(introspection_tail)];
    snprintf(xml, sizeof(xml), "%s%s%s", introspection_head,
	     (sink.set_state != NULL) ? introspection_actuator : "",
	     introspection_tail);

    const char  *s     = xml;
    DBusMessage *reply = dbus_message_new_method_return(msg);

    if ((reply != NULL) &&
	! dbus_message_append_args(reply, DBUS_TYPE_STRING, &s,
				   DBUS_TYPE_INVALID)) {
	dbus_message_unref(reply);
	reply = NULL;
    }
    send_reply(c, reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}


/*
 * SetState, on moses.Actuator. Only where a handler was given; anywhere
 * else it is left to libdbus, which answers UnknownMethod.
 *
 * The handler is called with the mutex released: the breaker's setter
 * emits the new state through put_data(), which comes straight back
 * into dbus_sink_put() and takes it.
 */
static DBusHandlerResult
on_set_state(DBusConnection *c, DBusMessage *msg)
{
    const char *state = NULL;
    DBusError   err;

    dbus_error_init(&err);
    if (! dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &state,
				DBUS_TYPE_INVALID)) {
	send_reply(c, dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					     err.message));
	dbus_error_free(&err);
	return DBUS_HANDLER_RESULT_HANDLED;
    }

    LOG("D-Bus: SetState(\"%s\") from %s", state,
	dbus_message_get_sender(msg) ? dbus_message_get_sender(msg) : "?");

    int rc = sink.set_state(state);
    if (rc == 0)
	send_reply(c, dbus_message_new_method_return(msg));
    else if (rc == -1)
	send_reply(c, dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					     "not a state"));
    else
	send_reply(c, dbus_message_new_error(msg, DBUS_ERROR_FAILED,
					     "the state could not be set"));
    return DBUS_HANDLER_RESULT_HANDLED;
}


static DBusHandlerResult
on_get(DBusConnection *c, DBusMessage *msg)
{
    const char *iface = NULL, *prop = NULL;
    DBusError   err;

    dbus_error_init(&err);
    if (! dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &iface,
				DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
	send_reply(c, dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					     err.message));
	dbus_error_free(&err);
	return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(iface, SINK_IFACE) != 0) {
	send_reply(c, dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_INTERFACE,
					     "no such interface here"));
	return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(prop, "Lines") != 0) {
	send_reply(c, dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY,
					     "no such property"));
	return DBUS_HANDLER_RESULT_HANDLED;
    }

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply != NULL) {
	DBusMessageIter it;
	dbus_message_iter_init_append(reply, &it);
	pthread_mutex_lock(&sink.mutex);
	bool ok = append_lines_variant(&it);
	pthread_mutex_unlock(&sink.mutex);
	if (! ok) {
	    dbus_message_unref(reply);
	    reply = NULL;
	}
    }
    send_reply(c, reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}


static DBusHandlerResult
on_get_all(DBusConnection *c, DBusMessage *msg)
{
    const char *iface = NULL;
    DBusError   err;

    dbus_error_init(&err);
    if (! dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &iface,
				DBUS_TYPE_INVALID)) {
	send_reply(c, dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					     err.message));
	dbus_error_free(&err);
	return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* An empty interface name asks for every property of every
     * interface, which is the same one property. */
    bool ours = (iface[0] == '\0') || (strcmp(iface, SINK_IFACE) == 0);

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply != NULL) {
	DBusMessageIter it, dict, entry;
	const char     *key = "Lines";
	bool            ok  = false;

	dbus_message_iter_init_append(reply, &it);
	if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}",
					     &dict)) {
	    ok = true;
	    if (ours) {
		pthread_mutex_lock(&sink.mutex);
		ok = dbus_message_iter_open_container(&dict,
						      DBUS_TYPE_DICT_ENTRY,
						      NULL, &entry)
		  && dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
						    &key)
		  && append_lines_variant(&entry)
		  && dbus_message_iter_close_container(&dict, &entry);
		pthread_mutex_unlock(&sink.mutex);
	    }
	    if (ok)
		ok = dbus_message_iter_close_container(&it, &dict);
	    else
		dbus_message_iter_abandon_container(&it, &dict);
	}
	if (! ok) {
	    dbus_message_unref(reply);
	    reply = NULL;
	}
    }
    send_reply(c, reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}


/*
 * Every message addressed to the object. Runs on the bus thread.
 *
 * Anything not answered here is left to libdbus, which replies
 * UnknownMethod to a method call it was handed back. Ping and
 * GetMachineId on org.freedesktop.DBus.Peer never reach this at all;
 * the library answers them itself.
 */
static DBusHandlerResult
on_message(DBusConnection *c, DBusMessage *msg, void *data)
{
    (void)data;

    if (dbus_message_is_method_call(msg, DBUS_INTERFACE_INTROSPECTABLE,
				    "Introspect"))
	return on_introspect(c, msg);
    if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "Get"))
	return on_get(c, msg);
    if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "GetAll"))
	return on_get_all(c, msg);
    if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "Set")) {
	/* Nothing on this bus writes a reading into a daemon. */
	send_reply(c, dbus_message_new_error(msg,
					     DBUS_ERROR_PROPERTY_READ_ONLY,
					     "readings are read-only"));
	return DBUS_HANDLER_RESULT_HANDLED;
    }
    if ((sink.set_state != NULL) &&
	dbus_message_is_method_call(msg, SINK_ACTUATOR_IFACE, "SetState"))
	return on_set_state(c, msg);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


//== The bus thread ====================================================

/*
 * Reach the bus, take the name, register the object.
 *
 * @return the connection, or NULL with the reason logged -- once, so
 *         that a bus that stays away does not fill the log every
 *         SINK_RETRY_S seconds
 */
static DBusConnection *
sink_connect(void)
{
    static bool reported = false;
    DBusError   err;

    dbus_error_init(&err);

    DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    if (c == NULL) {
	if (! reported)
	    LOG("D-Bus: cannot reach the system bus: %s", err.message);
	reported = true;
	dbus_error_free(&err);
	return NULL;
    }

    /* The default for a bus connection is to exit() the program when
     * the bus goes away. A valve controller does not stop holding the
     * valve because dbus-daemon restarted. */
    dbus_connection_set_exit_on_disconnect(c, FALSE);

    /* DO_NOT_QUEUE: this daemon is either the owner or is not, and
     * "queued behind another moses_breaker" is not a state worth
     * being in. Refused by the bus's policy when dbus/moses.conf is
     * not installed, and by a twin of this daemon still holding it. */
    int rc = dbus_bus_request_name(c, sink.name,
				   DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
	if (! reported) {
	    if (dbus_error_is_set(&err))
		LOG("D-Bus: cannot own %s: %s", sink.name, err.message);
	    else
		LOG("D-Bus: cannot own %s: already owned", sink.name);
	}
	reported = true;
	dbus_error_free(&err);
	dbus_connection_close(c);
	dbus_connection_unref(c);
	return NULL;
    }

    DBusObjectPathVTable vt = { .message_function = on_message };
    if (! dbus_connection_try_register_object_path(c, sink.path, &vt,
						   NULL, &err)) {
	if (! reported)
	    LOG("D-Bus: cannot serve %s: %s", sink.path, err.message);
	reported = true;
	dbus_error_free(&err);
	dbus_connection_close(c);
	dbus_connection_unref(c);
	return NULL;
    }

    if (reported)
	LOG("D-Bus: %s is on the system bus now", sink.name);
    reported = false;
    return c;
}


/* Wait out a retry interval, unless asked to stop meanwhile. */
static void
sink_backoff(void)
{
    for (int i = 0 ; (i < SINK_RETRY_S * 10) && ! sink.stopping ; i++)
	usleep(100 * 1000);
}


static void *
sink_thread(void *arg)
{
    (void)arg;

    while (! sink.stopping) {
	DBusConnection *c = sink_connect();
	if (c == NULL) {
	    sink_backoff();
	    continue;
	}

	pthread_mutex_lock(&sink.mutex);
	sink.conn = c;
	pthread_mutex_unlock(&sink.mutex);
	LOG("D-Bus                : %s at %s", sink.name, sink.path);

	/* Reads, dispatches to on_message(), and writes what the other
	 * thread queued. FALSE once the connection has dropped. */
	while (! sink.stopping &&
	       dbus_connection_read_write_dispatch(c, SINK_TICK_MS))
	    ;

	pthread_mutex_lock(&sink.mutex);
	sink.conn = NULL;
	pthread_mutex_unlock(&sink.mutex);

	/* Closing releases the name; the bus tells the consumers. A
	 * private connection is closed by whoever opened it. */
	dbus_connection_close(c);
	dbus_connection_unref(c);

	if (! sink.stopping) {
	    LOG("D-Bus: lost the system bus, retrying");
	    sink_backoff();
	}
    }
    return NULL;
}


//== Interface =========================================================

void
dbus_sink_on_set_state(dbus_sink_set_state_t handler)
{
    sink.set_state = handler;
}


void
dbus_sink_start(const char *daemon)
{
    if (sink.started)
	return;

    /* A bus name element: letters, digits, '_' and '-'. Anything else
     * would be refused by the bus every SINK_RETRY_S seconds for the
     * life of the daemon, so it is refused here, once, instead. */
    for (const char *p = daemon ; *p != '\0' ; p++) {
	if (! isalnum((unsigned char)*p) && (*p != '_') && (*p != '-')) {
	    LOG("D-Bus: \"%s\" is not a bus name element; not started",
		daemon);
	    return;
	}
    }
    if ((daemon[0] == '\0') || isdigit((unsigned char)daemon[0])) {
	LOG("D-Bus: \"%s\" is not a bus name element; not started", daemon);
	return;
    }

    int n = snprintf(sink.name, sizeof(sink.name), SINK_NAME_PREFIX "%s",
		     daemon);
    int m = snprintf(sink.path, sizeof(sink.path), SINK_PATH_PREFIX "%s",
		     daemon);
    if ((n < 0) || ((size_t)n >= sizeof(sink.name)) ||
	(m < 0) || ((size_t)m >= sizeof(sink.path))) {
	LOG("D-Bus: \"%s\" is too long for a bus name; not started", daemon);
	return;
    }

    /* Before any other libdbus call in the process. Idempotent, and
     * moses_display's source calls it too. */
    if (! dbus_threads_init_default()) {
	LOG("D-Bus: cannot initialise threading; not started");
	return;
    }

    sink.stopping = false;
    sink.nlines   = 0;

    int rc = pthread_create(&sink.thread, NULL, sink_thread, NULL);
    if (rc != 0) {
	errno = rc;
	LOG_ERRNO("D-Bus: cannot start the bus thread; not started");
	return;
    }
    sink.started = true;
}


void
dbus_sink_put(const char *line, size_t len)
{
    if (! sink.started)
	return;

    /* A copy with a NUL: put_data() hands over its buffer with the
     * newline still there, and a D-Bus string is NUL-terminated. */
    char copy[SINK_LINE_MAX];
    if (len >= sizeof(copy))
	len = sizeof(copy) - 1;
    memcpy(copy, line, len);
    copy[len] = '\0';

    pthread_mutex_lock(&sink.mutex);

    cache_put(copy);

    /* Signalled only when the bus is there and draining. Nothing is
     * queued for later: the cache is what a consumer that arrives
     * later reads. */
    DBusConnection *c = sink.conn;
    if ((c != NULL) &&
	(dbus_connection_get_outgoing_size(c) < SINK_BACKLOG_MAX)) {
	DBusMessage *sig = dbus_message_new_signal(sink.path, SINK_IFACE,
						   "Line");
	if (sig != NULL) {
	    const char *s = copy;
	    if (dbus_message_append_args(sig, DBUS_TYPE_STRING, &s,
					 DBUS_TYPE_INVALID))
		dbus_connection_send(c, sig, NULL);
	    dbus_message_unref(sig);
	}
    }

    pthread_mutex_unlock(&sink.mutex);
}


void
dbus_sink_stop(void)
{
    if (! sink.started)
	return;

    sink.stopping = true;
    pthread_join(sink.thread, NULL);

    pthread_mutex_lock(&sink.mutex);
    sink.nlines  = 0;
    sink.started = false;
    pthread_mutex_unlock(&sink.mutex);
}
