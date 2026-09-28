/*
 * moses_display -- what the screen shows
 *
 * The last known value of every figure on the screen, in one place.
 * Sources write it, the dashboard reads it, and neither knows the
 * other exists.
 *
 * It is locked because the writers are not the drawer: mosquitto
 * delivers messages on its own network thread (mqtt_start() calls
 * mosquitto_loop_start()), upsd is polled from another, and LVGL is not
 * thread safe. So a source calls model_set_*() from wherever it lives,
 * the dashboard takes a copy with model_get(), and no LVGL call is ever
 * made off the main thread.
 *
 * The lock is its own, and deliberately not LVGL's. lv_lock() is real
 * here (LV_USE_OS is LV_OS_PTHREAD, see src/lv_conf.h), but it is the
 * mutex lv_timer_handler() holds for a whole frame: writing the model
 * under it would park an arriving MQTT message behind a panel refresh
 * for no reason. A source takes this mutex for the length of one
 * assignment instead, and never touches LVGL at all.
 *
 * That indirection is the point rather than a side effect: a source
 * parses whatever it can reach and calls model_set_*(). The unix socket
 * of step two is a new file calling the same setters -- nothing here,
 * and nothing in the dashboard, has to change for it.
 */

#ifndef __DISPLAY_MODEL_H
#define __DISPLAY_MODEL_H

#include <stdbool.h>
#include <time.h>


/** Longest ups.status this keeps ("OL CHRG LB ..." and then some). */
#define MODEL_STATUS_MAX	32


/**
 * A daemon whose liveness is tracked, one per availability topic.
 *
 * Availability is per-daemon and retained (see docs/interfaces.md,
 * MQTT topics), so these are known within a moment of connecting
 * rather than after the first reading arrives.
 */
enum model_producer {
    MODEL_WATERMETER = 0,
    MODEL_BREAKER,
    MODEL_SENSORS,
    MODEL_PRODUCER_COUNT,
};

/**
 * How much is known about the UPS.
 *
 * Four states and not two, because "nothing heard yet" and "there is no
 * UPS" are different things and the panel must not say the second when
 * it means the first. Which of them applies depends on where the UPS is
 * being read from: upsd answers immediately or not at all, while the
 * MQTT events are edges -- upsmon sends one when something changes and
 * says nothing in between, so a display that has just started may wait
 * a long time before it legitimately knows anything.
 */
enum model_ups_state {
    MODEL_UPS_UNKNOWN = 0,	/**< nothing heard yet			*/
    MODEL_UPS_ABSENT,		/**< looked, and there is no UPS	*/
    MODEL_UPS_LOST,		/**< upsd is there and will not say	*/
    MODEL_UPS_KNOWN,		/**< a status, with or without figures	*/
};

/**
 * The watermeter's leak report (docs/leak.md), as far as the panel is
 * concerned: how bad, what kind, since when, how fast.
 *
 * The same words as the report itself -- src/leak.h's -- because the
 * panel shows what moses_watermeter concluded and concludes nothing of
 * its own.
 */
enum model_leak_level {
    MODEL_LEAK_OK = 0,
    MODEL_LEAK_WARN,
    MODEL_LEAK_ALERT,
};

enum model_leak_kind {
    MODEL_LEAK_NONE = 0,
    MODEL_LEAK_FLOW,		/**< water that does not stop		*/
    MODEL_LEAK_SLOW,		/**< a regular drip			*/
    MODEL_LEAK_QUIET,		/**< a house that never rests		*/
};

/** What an availability topic last said about a daemon. */
enum model_avail {
    MODEL_AVAIL_UNKNOWN = 0,	/**< nothing retained, nothing seen	*/
    MODEL_AVAIL_ONLINE,
    MODEL_AVAIL_OFFLINE,	/**< its last will, published for it	*/
};


/**
 * Every figure, with when it arrived.
 *
 * `known` says a value was ever seen; the timestamp says how long ago,
 * which is what tells a live reading from one left over from a daemon
 * that has since gone quiet. Times are CLOCK_REALTIME seconds, because
 * they are compared against readings stamped by other machines.
 */
struct model {
    struct {				/* the solenoid valve		*/
	bool	 known;
	bool	 closed;		/**< relay energised: water shut */
	time_t	 at;
    } valve;

    struct {				/* meter index, in litres	*/
	bool	 known;
	double	 litres;
	time_t	 at;
    } index;

    struct {				/* GPIO pulse counting		*/
	bool	 known;			/**< a report has arrived	*/
	unsigned long count;		/**< pulses in that report	*/
	time_t	 at;			/**< when it arrived		*/
	time_t	 latched_at;		/**< when one was last non-zero	*/
    } pulse;

    struct {				/* BME280			*/
	bool	 known;
	double	 celsius;
	time_t	 at;
    } temperature;

    struct {				/* the UPS			*/
	enum model_ups_state state;
	char	 status[MODEL_STATUS_MAX];	/**< ups.status		*/
	double	 charge;		/**< percent, < 0 when absent	*/
	double	 runtime;		/**< seconds, < 0 when unknown	*/
	bool	 estimated;		/**< runtime computed, not read	*/
	time_t	 at;
    } ups;

    struct {				/* moses_watermeter --leak	*/
	bool	 known;
	enum model_leak_level level;
	enum model_leak_kind  kind;
	time_t	 since;			/**< when the signature began, 0 = none */
	double	 rate;			/**< L/min (flow), L/h (slow), else 0 */
	time_t	 at;
    } leak;

    enum model_avail avail[MODEL_PRODUCER_COUNT];
};


/**
 * Prepare the model. Call once, before any source is started.
 */
void model_init(void);

/**
 * Copy the model out, consistently.
 *
 * Everything the dashboard draws comes from one such copy, so a screen
 * never mixes a figure from before a message with one from after it.
 */
void model_get(struct model *out);

/*
 * The setters. Safe from any thread, and each one stamps the field it
 * writes with the current time.
 */
void model_set_valve(bool closed);
void model_set_index(double litres);
void model_set_pulse(unsigned long count);
void model_set_temperature(double celsius);
void model_set_avail(enum model_producer who, bool online);

/*
 * The same, stamped with a time the caller vouches for. For a reading
 * read back later than it was taken -- the bus keeps the last line of
 * each kind for a display that starts between reports -- so that it
 * ages from when it was really taken rather than from when it was
 * found. CLOCK_REALTIME seconds, like `at` everywhere above.
 */
void model_set_valve_at(bool closed, time_t at);
void model_set_index_at(double litres, time_t at);
void model_set_pulse_at(unsigned long count, time_t at);
void model_set_temperature_at(double celsius, time_t at);

/**
 * Record a leak report. Published retained and only when it changes, so
 * it is not aged like a reading: it stands until the next one, or until
 * its producer is gone (docs/leak.md, *What is published*).
 */
void model_set_leak(enum model_leak_level level, enum model_leak_kind kind,
		    time_t since, double rate);
void model_set_leak_at(enum model_leak_level level, enum model_leak_kind kind,
		       time_t since, double rate, time_t at);

/** The report's words for level and kind, back to the enums; -1 for a
 *  word this panel does not know, so a report it cannot read is
 *  refused rather than shown as something else. */
int model_leak_level_parse(const char *word);
int model_leak_kind_parse(const char *word);

/**
 * Record a UPS reading. `charge` and `runtime` are negative when the
 * UPS does not report them; `estimated` says runtime was computed here
 * rather than read from upsd.
 */
void model_set_ups(const char *status, double charge, double runtime,
		   bool estimated);

/**
 * Record that upsd is there but would not say anything useful.
 *
 * Distinct from never having answered: a UPS being watched that cannot
 * be read is a fault, whereas a machine with no upsd at all is not.
 */
void model_set_ups_lost(void);

/**
 * Note that upsd could not be reached at all.
 */
void model_set_ups_absent(void);

/**
 * Record a UPS state learned from a nut-notify event.
 *
 * `type` is the NOTIFYTYPE out of the topic -- ONLINE, ONBATT, LOWBATT,
 * SHUTDOWN and friends. These carry no charge and no remaining time, so
 * the figures are left unknown and only the status is set; the screen
 * then says what the UPS is doing without inventing a number for how
 * long it can keep doing it.
 */
void model_set_ups_event(const char *type);

#endif
