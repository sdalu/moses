/*
 * dbus_sink.h -- each reading on the system bus
 *
 * The producing half of WITH_DBUS. A daemon that starts this owns a
 * name on the system bus for as long as it runs, keeps the last line of
 * each kind it emitted, and hands both to whoever asks:
 *
 *     bus name    moses.<daemon>          moses.watermeter, moses.breaker,
 *                                         moses.sensors
 *     object      /moses/<daemon>
 *     interface   moses.Readings
 *       property  Lines  as   read        the last line of each kind
 *       signal    Line   s                every line, as it is emitted
 *     interface   moses.Actuator           moses.breaker only
 *       method    SetState (s state)      state/set, without the broker
 *
 * What travels is the same line of InfluxDB line protocol the stdout
 * sink writes (common.h, put_data()), without its newline. Not a second
 * format: the display already reads that one, and busctl shows it as
 * it stands.
 *
 * "Each kind" is a measurement and its first field -- `watermeter
 * index=`, `watermeter pulse=`, `environment temperature=`, `breaker
 * state=`, and `<measurement> failure=` -- so a consumer that connects
 * late gets the last index and the last pulse count, not merely
 * whichever line was emitted last. That is the retention a socket did
 * not have. The name is the liveness: the bus tells everyone watching
 * the moment its owner goes, which is the last will a socket did not
 * have either.
 *
 * Nothing here can hold a daemon up. Sending queues a message and
 * returns; a bus that has stopped draining is noticed by the size of
 * that queue and further signals are dropped rather than accumulated.
 * A bus that is not there, or refuses the name, is logged once and
 * retried from a thread of its own, and until it is there the readings
 * are kept so the first consumer still gets them. The daemon never
 * waits on any of it.
 *
 * The thread is the only place the bus is read: it answers the
 * property calls out of the cache and notices the connection dropping.
 * dbus_sink_put() is called from wherever a reading is taken and only
 * ever writes the cache and queues a signal.
 */

#ifndef __DBUS_SINK_H
#define __DBUS_SINK_H

#include <stddef.h>

#ifdef WITH_DBUS

/**
 * Own moses.<daemon> on the system bus and serve /moses/<daemon>, from a
 * thread of its own. Never fails the caller: a bus that cannot be
 * reached is retried, and a reading emitted meanwhile is kept for when
 * it can. `daemon` is one of "watermeter", "breaker", "sensors" -- a
 * bus name element, so letters, digits, '_' and '-' only.
 */
void dbus_sink_start(const char *daemon);

/**
 * One line, without its newline, as put_data() built it. Kept as the
 * last of its kind and signalled to whoever is listening.
 */
void dbus_sink_put(const char *line, size_t len);

/**
 * Leave the bus. The name is released, which is what tells a consumer
 * the daemon has gone; the cache is dropped. The daemons never call
 * this -- they leave the bus by exiting, and the bus says so for them --
 * but a test starts and stops a producer several times.
 */
void dbus_sink_stop(void);

/**
 * A SetState command, as a caller on the bus sent it: the same text
 * the `state/set` topic takes. Returns 0 once applied, -1 when the text
 * is not a state at all (the caller is told InvalidArgs), and -2 when
 * applying it failed (the caller is told Failed). Runs on the bus
 * thread, so it must not wait on anything the bus thread holds.
 */
typedef int (*dbus_sink_set_state_t)(const char *state);

/**
 * Take SetState commands on moses.Actuator, handing each to `handler`.
 * Before dbus_sink_start(). Only moses_breaker has anything to set, so
 * the other daemons never call this and the interface does not exist on
 * their objects: a SetState sent to them is UnknownMethod. Who may send
 * one at all is the bus's policy (dbus/moses.conf): root, and nobody
 * else.
 */
void dbus_sink_on_set_state(dbus_sink_set_state_t handler);

#define DBUS_SINK_START(daemon)		dbus_sink_start(daemon)

#else

/* Without the bus, the one call a daemon makes compiles to nothing. */
#define DBUS_SINK_START(daemon)		do { } while (0)

#endif

#endif
