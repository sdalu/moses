/*
 * moses_display -- where the figures come from
 *
 * A source reads something and writes the model (src/display/model.h).
 * That is the whole of the contract: it is handed what it needs to
 * reach its data, it runs until the program ends, and it never draws.
 *
 * There are three, and they differ in kind rather than in interface:
 *
 *   source-mqtt  what the other moses daemons publish -- the valve, the
 *                meter index, the pulse counter and the temperature.
 *                Pushed: mosquitto's thread calls back on arrival.
 *
 *   source-nut   the UPS, asked of upsd on the loopback in its own line
 *                protocol. Pulled on an interval from a thread of its
 *                own, and started only when --ups names a UPS: upsd
 *                publishes no state to MQTT, so it is the only way to a
 *                charge and a remaining time, and it only exists on the
 *                machine the UPS is attached to. Without --ups the MQTT
 *                source takes the UPS on itself, from nut-notify's
 *                events.
 *
 *   source-dbus  the same readings as source-mqtt, off the system bus,
 *                without a broker in between -- built only under
 *                WITH_DBUS, since that is what puts the daemons on the
 *                bus. Pushed, from a thread blocked in the bus, and
 *                fetched once at startup: each daemon keeps the last
 *                line of each kind it emitted, and the bus says when a
 *                daemon's name is taken or dropped. So it has the
 *                retention and the liveness MQTT has, without the
 *                network; see source-dbus.c for what it will not claim.
 *
 * All three write the model and nothing else, which is what lets them be
 * three files rather than three special cases inside main().
 */

#ifndef __DISPLAY_SOURCE_H
#define __DISPLAY_SOURCE_H

#include <stdbool.h>

struct mqtt;				// common.h


/**
 * Subscribe to what the daemons of one installation publish.
 *
 * Topics are built from mqtt_topic_prefix(), which is already how a
 * broker carrying more than one installation tells them apart --
 * `water-breaker/moses`.
 *
 * `ups` names the UPS whose nut-notify events to watch, or is NULL to
 * watch every UPS on the broker. `ups_from_events` says whether those
 * events are the only thing that knows about the UPS -- true when
 * nothing is polling upsd, in which case an event sets the UPS state
 * rather than merely hurrying the next poll along.
 *
 * Returns 0 once connected, or when MQTT is not configured at all --
 * the screen is then simply empty, which is worth seeing. < 0 on error.
 */
int source_mqtt_start(struct mqtt *handler, const char *ups,
		      bool ups_from_events);

/**
 * Start polling upsd every `interval` seconds, on its own thread.
 *
 * `ups` names the UPS, or is NULL to take the first one upsd lists.
 *
 * @return < 0 if the thread could not be started
 */
int source_nut_start(const char *ups, unsigned long interval);

/**
 * Ask upsd once, before anything is started.
 *
 * `ups` names the UPS, or is NULL to take the first one upsd lists.
 *
 * Puts a first reading on the panel rather than leaving it blank until
 * one interval has passed, and settles the name so the MQTT source can
 * subscribe to that UPS's events in particular. Not an error when there
 * is no upsd: it is then reported as absent, which the screen says
 * plainly.
 */
void source_nut_probe(const char *ups);

/**
 * The UPS upsd is being asked about, discovered if it was not named.
 *
 * Only meaningful after source_nut_probe(); NULL when there is no upsd
 * or it knows no UPS.
 */
const char *source_nut_name(void);


#ifdef WITH_DBUS
/**
 * Reach the system bus and watch the daemons on it, on its own thread.
 *
 * Asks each daemon that is there for the last line of each kind it
 * emitted, then takes every line as it is emitted, and marks a daemon
 * online when its name is owned and offline when its name is seen to
 * be dropped. A name never seen is left alone.
 *
 * The bus is DBUS_BUS_SYSTEM, which libdbus locates itself; setting
 * DBUS_SYSTEM_BUS_ADDRESS in the environment points it elsewhere, which
 * is how test/test_dbus.c runs this against a private bus.
 *
 * @return < 0 if the bus could not be reached or the thread not started
 */
int source_dbus_start(void);

/**
 * Leave the bus and stop reading. Called on the way out.
 */
void source_dbus_stop(void);
#endif

#endif
