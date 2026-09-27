/*
 * moses_display -- where the figures come from
 *
 * A source reads something and writes the model (src/display/model.h).
 * That is the whole of the contract: it is handed what it needs to
 * reach its data, it runs until the program ends, and it never draws.
 *
 * There are two, and they differ in kind rather than in interface:
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
 *   source-unix  the same readings as source-mqtt, off a local unix
 *                datagram socket, without a broker in between -- built
 *                only under WITH_DGRAM, since that is what makes the
 *                daemons send them. Pushed, from a thread blocked in
 *                recv(). It carries no retained state and no last will,
 *                so it is a shortcut that survives the network being
 *                gone rather than a replacement for MQTT; see
 *                source-unix.c for what that costs.
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


#ifdef WITH_DGRAM
/**
 * Bind the local datagram socket and read it, on its own thread.
 *
 * The path is dgram_path() -- $MOSES_DGRAM_PATH, or the compiled-in
 * MOSES_DGRAM_PATH. This is the end that binds; the daemons only ever
 * send.
 *
 * Refuses rather than takes the address when the path is an existing
 * non-socket file, or when another consumer is already bound to it: both
 * would otherwise end with one of the two receiving nothing and neither
 * of them saying so.
 *
 * @return < 0 if the socket could not be bound or the thread not started
 */
int source_unix_start(void);

/**
 * Remove the socket file and stop reading.
 *
 * Called on the way out, so the next run does not find a stale socket
 * and have to work out whether anything is still using it.
 */
void source_unix_stop(void);
#endif

#endif
