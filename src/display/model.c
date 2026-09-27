/*
 * moses_display -- the shared model
 *
 * A struct and a mutex. Every setter takes the lock, writes its own
 * field and stamps it; model_get() copies the whole thing out under the
 * same lock. Nothing here blocks on anything but the lock, so a source
 * thread never waits on the drawing and the drawing never waits on the
 * network.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "model.h"


static struct model	model;
static pthread_mutex_t	model_mutex = PTHREAD_MUTEX_INITIALIZER;


/* Now, on the clock the readings are stamped with. */
static time_t
model_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}


void
model_init(void)
{
    pthread_mutex_lock(&model_mutex);
    memset(&model, 0, sizeof(model));
    model.ups.charge  = -1;
    model.ups.runtime = -1;
    pthread_mutex_unlock(&model_mutex);
}


void
model_get(struct model *out)
{
    pthread_mutex_lock(&model_mutex);
    *out = model;
    pthread_mutex_unlock(&model_mutex);
}


void
model_set_valve_at(bool closed, time_t at)
{
    pthread_mutex_lock(&model_mutex);
    model.valve.known  = true;
    model.valve.closed = closed;
    model.valve.at     = at;
    pthread_mutex_unlock(&model_mutex);
}


void
model_set_valve(bool closed)
{
    model_set_valve_at(closed, model_now());
}


void
model_set_index_at(double litres, time_t at)
{
    pthread_mutex_lock(&model_mutex);
    model.index.known  = true;
    model.index.litres = litres;
    model.index.at     = at;
    pthread_mutex_unlock(&model_mutex);
}


void
model_set_index(double litres)
{
    model_set_index_at(litres, model_now());
}


/*
 * A pulse report.
 *
 * The watermeter publishes the count seen since its last report, and a
 * 0 as a heartbeat when --idle-timeout passes with nothing
 * (docs/interfaces.md, MQTT topics). So a report is not itself news; a
 * non-zero one is, and its time is kept separately as the latch the
 * dashboard holds the flow marker on. Without that the marker would be
 * a one-frame flicker on a screen redrawn thirty times a second.
 */
void
model_set_pulse_at(unsigned long count, time_t at)
{
    pthread_mutex_lock(&model_mutex);
    model.pulse.known = true;
    model.pulse.count = count;
    model.pulse.at    = at;
    if (count > 0)
	model.pulse.latched_at = model.pulse.at;
    pthread_mutex_unlock(&model_mutex);
}


void
model_set_pulse(unsigned long count)
{
    model_set_pulse_at(count, model_now());
}


void
model_set_temperature_at(double celsius, time_t at)
{
    pthread_mutex_lock(&model_mutex);
    model.temperature.known   = true;
    model.temperature.celsius = celsius;
    model.temperature.at      = at;
    pthread_mutex_unlock(&model_mutex);
}


void
model_set_temperature(double celsius)
{
    model_set_temperature_at(celsius, model_now());
}


void
model_set_ups(const char *status, double charge, double runtime,
	      bool estimated)
{
    pthread_mutex_lock(&model_mutex);
    model.ups.state     = MODEL_UPS_KNOWN;
    model.ups.charge    = charge;
    model.ups.runtime   = runtime;
    model.ups.estimated = estimated;
    model.ups.at        = model_now();
    snprintf(model.ups.status, sizeof(model.ups.status), "%s",
	     (status != NULL) ? status : "");
    pthread_mutex_unlock(&model_mutex);
}


void
model_set_ups_lost(void)
{
    pthread_mutex_lock(&model_mutex);
    model.ups.state      = MODEL_UPS_LOST;
    model.ups.charge     = -1;
    model.ups.runtime    = -1;
    model.ups.estimated  = false;
    model.ups.status[0]  = '\0';
    model.ups.at         = model_now();
    pthread_mutex_unlock(&model_mutex);
}


void
model_set_ups_absent(void)
{
    pthread_mutex_lock(&model_mutex);
    model.ups.state      = MODEL_UPS_ABSENT;
    model.ups.charge     = -1;
    model.ups.runtime    = -1;
    model.ups.estimated  = false;
    model.ups.status[0]  = '\0';
    model.ups.at         = model_now();
    pthread_mutex_unlock(&model_mutex);
}


/*
 * A nut-notify event, turned into a status.
 *
 * upsmon names a transition, not a state, so this is the translation
 * back: what the UPS must be doing for that event to have been sent.
 * The words are NUT's own ups.status flags, so everything downstream --
 * ups_on_battery(), condense_status() -- reads them exactly as it reads
 * the ones upsd reports, and there is no second vocabulary.
 *
 * An unrecognised type is kept verbatim rather than dropped: a NUT that
 * grows a new notification should put it on the panel, not vanish.
 */
void
model_set_ups_event(const char *type)
{
    static const struct { const char *type; const char *status; } map[] = {
	{ "ONLINE",   "OL"    },
	{ "ONBATT",   "OB"    },
	{ "LOWBATT",  "OB LB" },	/* upsmon sends it while on battery */
	{ "SHUTDOWN", "FSD"   },
	{ "REPLBATT", "RB"    },
	{ "COMMOK",   "OL"    },
    };

    if (type == NULL)
	return;

    /* Comms lost is not a state of the UPS but of the watching of it,
     * and it is the one case where something is wrong and no status can
     * be believed. */
    if ((strcmp(type, "NOCOMM")  == 0) ||
	(strcmp(type, "COMMBAD") == 0)) {
	model_set_ups_lost();
	return;
    }

    const char *status = type;
    for (size_t i = 0 ; i < sizeof(map) / sizeof(map[0]) ; i++) {
	if (strcmp(type, map[i].type) == 0) {
	    status = map[i].status;
	    break;
	}
    }

    pthread_mutex_lock(&model_mutex);
    model.ups.state     = MODEL_UPS_KNOWN;
    model.ups.charge    = -1;		/* an event carries no figures */
    model.ups.runtime   = -1;
    model.ups.estimated = false;
    model.ups.at        = model_now();
    snprintf(model.ups.status, sizeof(model.ups.status), "%s", status);
    pthread_mutex_unlock(&model_mutex);
}


void
model_set_avail(enum model_producer who, bool online)
{
    /* Cast: an enum with no negative enumerator may be unsigned, and
     * comparing that against 0 is a warning rather than a check. */
    if (((int)who < 0) || (who >= MODEL_PRODUCER_COUNT))
	return;

    pthread_mutex_lock(&model_mutex);
    model.avail[who] = online ? MODEL_AVAIL_ONLINE : MODEL_AVAIL_OFFLINE;
    pthread_mutex_unlock(&model_mutex);
}
