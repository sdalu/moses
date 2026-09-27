/*
 * moses_display -- the MQTT source
 *
 * Subscribes to what the other three daemons publish and writes each
 * message into the model. Nothing is published from here: the display
 * watches, it does not take part.
 *
 * Topics are the ones docs/interfaces.md, *MQTT topics*, documents,
 * relative to MQTT_TOPIC_PREFIX like everywhere else -- which is
 * already what tells one installation from another on a shared broker.
 * `index` and `state` are retained by their producers, and so is each
 * `availability/<daemon>` last will, so most of the screen is filled
 * within a moment of connecting rather than after the first interval
 * elapses. `pulse` and `sensors` are not retained and arrive on their
 * producer's interval.
 *
 * The UPS is the exception: nut-notify publishes outside the prefix
 * (docs/system.md, *Nut*), and what is subscribed to there is its
 * retained `ups/<ups>/state` rather than the per-type notification
 * topics. upsmon speaks only on a change, so the retained state is the
 * only thing a display that has just started can read. Whether it is
 * the whole of what this knows about the UPS depends on --ups; see
 * notify_owns_ups below.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <mosquitto.h>

#include "common.h"
#include "breaker_state.h"

#include "model.h"
#include "payload.h"
#include "source.h"


/* Subscribing at QoS 0: every one of these is a periodically re-sent
 * reading or a retained value, so a message lost in transit is replaced
 * by the next one. Nothing here is a command and nothing is counted. */
#define SUB_QOS		0

/*
 * The topics, built once from the prefix.
 *
 * Kept as a table so the subscribe list and the dispatch cannot drift
 * apart: both walk this array, and a topic without a handler would not
 * compile.
 */
enum sub {
    SUB_INDEX = 0,
    SUB_PULSE,
    SUB_STATE,
    SUB_SENSORS,
    SUB_AVAIL_WATERMETER,
    SUB_AVAIL_BREAKER,
    SUB_AVAIL_SENSORS,
    SUB_COUNT,
};

static const char *const sub_suffix[SUB_COUNT] = {
    [SUB_INDEX]            = "index",
    [SUB_PULSE]            = "pulse",
    [SUB_STATE]            = "state",
    [SUB_SENSORS]          = "sensors",
    [SUB_AVAIL_WATERMETER] = "availability/watermeter",
    [SUB_AVAIL_BREAKER]    = "availability/breaker",
    [SUB_AVAIL_SENSORS]    = "availability/sensors",
};

static char *sub_topic[SUB_COUNT];

/* The unprefixed UPS state topic, wildcarded over the UPS name when it
 * is not known. */
static char *state_topic;

/* Whether these events are the only thing that knows about the UPS.
 *
 * True when no --ups was named, so nothing is polling upsd and the
 * events are the whole story; an event then sets the UPS state. False
 * when upsd is being polled, where the events only make a change show
 * up sooner than the next poll would have, and the figures that poll
 * brings back must not be thrown away by an edge that carries none. */
static bool notify_owns_ups;


//== Payload parsing ===================================================

/*
 * The parsers themselves are payload.c, which knows nothing of
 * mosquitto. What is left here is the one line of glue: a mosquitto
 * message carries its length separately and its buffer is not
 * NUL-terminated, which is exactly the shape those take.
 */
#define PAYLOAD(msg)	(const char *)(msg)->payload, (size_t)(msg)->payloadlen

/* True when the message has a payload at all. mosquitto uses a signed
 * length, so a negative one is refused before it becomes a size_t. */
static bool
has_payload(const struct mosquitto_message *msg)
{
    return (msg->payload != NULL) && (msg->payloadlen > 0);
}


/*
 * A breaker state payload.
 *
 * moses_breaker echoes 0 or 1 on `state` (docs/interfaces.md,
 * *MQTT topics*), but this is breaker_parse_state() -- the very parser
 * moses_breaker reads its `state/set` commands with -- rather than a
 * second one written here. One vocabulary, so the display cannot
 * disagree with the valve about what a payload meant, and it arrives
 * already covered by test/test_breaker_state.c.
 */
static bool
payload_bool(const struct mosquitto_message *msg, bool *val)
{
    if (msg->payload == NULL)
	return false;

    int state = breaker_parse_state(msg->payload, msg->payloadlen);
    if (state < 0)
	return false;

    *val = (state != 0);
    return true;
}


//== Dispatch ==========================================================

/*
 * A message arrived. Runs on mosquitto's network thread, so it does
 * nothing but parse and call a setter -- no LVGL, no blocking.
 */
static void
on_message(struct mosquitto *mosq, void *obj,
	   const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)obj;

    if (msg->topic == NULL)
	return;

    /* The UPS state topic may be a wildcard, so it is matched first and
     * by shape rather than by string equality. */
    if (state_topic != NULL) {
	bool matched = false;
	if ((mosquitto_topic_matches_sub(state_topic, msg->topic,
					&matched) == MOSQ_ERR_SUCCESS) &&
	    matched) {
	    char type[MODEL_STATUS_MAX];
	    if (has_payload(msg) && payload_text(PAYLOAD(msg), type,
						 sizeof(type))) {
		LOG("MQTT ups state       : %s", type);
		if (notify_owns_ups)
		    model_set_ups_event(type);
	    }
	    return;
	}
    }

    for (int i = 0 ; i < SUB_COUNT ; i++) {
	if ((sub_topic[i] == NULL) || (strcmp(sub_topic[i], msg->topic) != 0))
	    continue;

	switch ((enum sub)i) {
	case SUB_INDEX: {
	    double litres;
	    if (has_payload(msg) && payload_double(PAYLOAD(msg), &litres))
		model_set_index(litres);
	    else
		LOG("garbage content for MQTT topic %s", msg->topic);
	    return;
	}
	case SUB_PULSE: {
	    unsigned long count;
	    if (has_payload(msg) && payload_ulong(PAYLOAD(msg), &count))
		model_set_pulse(count);
	    else
		LOG("garbage content for MQTT topic %s", msg->topic);
	    return;
	}
	case SUB_STATE: {
	    bool closed;
	    if (payload_bool(msg, &closed))
		model_set_valve(closed);
	    else
		LOG("garbage content for MQTT topic %s", msg->topic);
	    return;
	}
	case SUB_SENSORS: {
	    double celsius;
	    if (has_payload(msg) &&
		payload_json_number(PAYLOAD(msg), "temperature", &celsius))
		model_set_temperature(celsius);
	    else
		LOG("garbage content for MQTT topic %s", msg->topic);
	    return;
	}
	case SUB_AVAIL_WATERMETER:
	case SUB_AVAIL_BREAKER:
	case SUB_AVAIL_SENSORS: {
	    /* Same order as the three SUB_AVAIL_* enumerators, which are
	     * adjacent so one subtraction indexes this. */
	    static const enum model_producer who[] = {
		MODEL_WATERMETER, MODEL_BREAKER, MODEL_SENSORS,
	    };
	    bool online;
	    if (has_payload(msg) && payload_online(PAYLOAD(msg), &online))
		model_set_avail(who[i - SUB_AVAIL_WATERMETER], online);
	    return;
	}
	case SUB_COUNT:
	    return;
	}
    }
}


//== Source ============================================================

int
source_mqtt_start(struct mqtt *handler, bool readings,
		  const char *ups, bool ups_from_events)
{
    const char *prefix = mqtt_topic_prefix();

    /* Build the prefixed topics, then the subscription list from the
     * same array -- one list, so a topic cannot be subscribed to
     * without being dispatched or the other way round. */
    struct mqtt_subscription sub[SUB_COUNT + 1];
    unsigned int             subcount = 0;

    for (int i = 0 ; readings && (i < SUB_COUNT) ; i++) {
	if (asprintf(&sub_topic[i], "%s/%s", prefix, sub_suffix[i]) < 0)
	    DIE(2, "failed to allocate MQTT topic string");
	sub[subcount++] = (struct mqtt_subscription){
	    .topic = sub_topic[i],
	    .qos   = SUB_QOS,
	};
	LOG("MQTT subscribe       : %s", sub_topic[i]);
    }

    /* The UPS state. nut-notify ignores MQTT_TOPIC_PREFIX, so this one
     * is absolute (docs/system.md, *Nut*), and it is retained -- which
     * is the whole point of subscribing to it rather than to the
     * per-type notification topics. upsmon speaks only when something
     * changes, so on a machine that has been on mains for a year there
     * is no event to catch; the retained state is there to be read at
     * once.
     *
     * With a UPS known -- named on the command line or found by asking
     * upsd -- only that one is watched, so a second UPS on the same
     * broker cannot write over it. Otherwise the name is not known and
     * the subscription wildcards over it. */
    notify_owns_ups = ups_from_events;
    if (asprintf(&state_topic, "ups/%s/state",
		 (ups != NULL) ? ups : "+") < 0)
	DIE(2, "failed to allocate MQTT topic string");
    sub[subcount++] = (struct mqtt_subscription){
	.topic = state_topic,
	.qos   = SUB_QOS,
    };
    LOG("MQTT subscribe       : %s%s", state_topic,
	notify_owns_ups ? "  (UPS state comes from this)" : "");

    /* No availability topic of its own: this program publishes nothing,
     * and a display being up says nothing about the water. */
    int rc = mqtt_connect(handler, subcount, sub, NULL, on_message);
    if (rc < 0)
	return -1;
    if (rc > 0)
	LOG("MQTT connection established");

    return 0;
}
