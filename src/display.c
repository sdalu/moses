/*
 * moses_display -- show the installation on the HAT's LCD.
 *
 * A read-only front panel for the other three daemons. It subscribes to
 * what they publish, asks upsd about the battery, and draws the lot on
 * the 0.96" 160x80 LCD of the Automation HAT Mini -- the valve, the
 * meter index with a marker when water is moving, the temperature, and
 * the UPS with how long it has left.
 *
 * It publishes nothing and commands nothing. The relay on that same HAT
 * is the solenoid valve and belongs to moses_breaker; this program
 * takes only the panel's three pins (docs/hardware.md, *LCD*) and
 * never goes near it.
 *
 * Three threads. This one owns LVGL and does nothing else; mosquitto's
 * network thread and the upsd poller only ever write the model
 * (src/display/model.h), which this one copies out once a second to put
 * on the screen. That is the whole of the concurrency, and it is why
 * neither a slow broker nor an unresponsive upsd can stall a repaint.
 *
 * Unlike the other daemons there is no --reduced-latency: this one has
 * no deadline to miss, and putting a screen refresh on SCHED_FIFO on a
 * single-core Pi would be competing with the program that shuts the
 * water off.
 *
 * All MQTT topics are relative to MQTT_TOPIC_PREFIX, except the UPS
 * notifications, which nut-notify publishes outside it.
 *
 * The UPS has two ways in and --ups picks between them. With it, upsd
 * on this machine is polled -- for the UPS named, or the first it lists
 * -- and the panel gets a charge and a remaining time. Without it, the
 * state comes from nut-notify's MQTT events alone, which is what a
 * display running anywhere else can have.
 */

#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"

#include "common.h"

#include "display/backend.h"
#include "display/dashboard.h"
#include "display/model.h"
#include "display/source.h"


/* How often the screen is brought up to date. The figures behind it
 * move on the order of a minute and the clock shows whole minutes, so
 * this is already generous; dashboard_update() writes nothing when
 * nothing changed, so a tick that finds no news costs no redraw. */
#define UPDATE_PERIOD_MS	1000

/* How often upsd is asked. It is a socket on the loopback and the
 * answer is a couple of dozen short lines. */
#define UPS_INTERVAL		10	/* seconds			*/

/* How long a published reading stays believable without being
 * repeated. The daemons report on a one-minute interval in the
 * deployed configuration, and the breaker heartbeats at the same rate,
 * so this is two and a half missed reports. */
#define STALE_AFTER		150	/* seconds			*/


/* Where the daemons' readings are read from. See main(). */
enum source {
    SOURCE_AUTO = 0,
    SOURCE_MQTT,
    SOURCE_DBUS,
};


struct display {
    struct mqtt   mqtt;
    enum source   source;		/**< --source			*/
    bool          use_upsd;		/**< --ups given: poll upsd here */
    char         *ups;			/**< the UPS, NULL to discover	*/
    unsigned long ups_interval;
    unsigned long stale_after;
    bool          check_only;		/**< --check: stop once it is up */
};


/*
 * __progname is not defined here, unlike in the three daemons.
 *
 * Both libcs this is built against already provide it -- and on FreeBSD
 * it lives in crt1.o, which is linked into every program, so defining
 * another is a duplicate symbol and not merely a shadow of the one in
 * libc. Assigning to it in main() is all that is wanted, and it has the
 * side benefit that anything in libc which reports a program name
 * agrees with what DIE() prints.
 */

static struct display display = {
    .mqtt         = MQTT_INITIALIZER(),
    .source       = SOURCE_AUTO,
    .use_upsd     = false,
    .ups          = NULL,
    .ups_interval = UPS_INTERVAL,
    .stale_after  = STALE_AFTER,
};

static volatile sig_atomic_t running = 1;


static void
on_signal(int sig)
{
    (void)sig;
    running = 0;
}


/*
 * One update, from inside lv_timer_handler().
 *
 * Runs on the LVGL thread with LVGL's own lock held, which is what
 * makes it safe to touch widgets here and nowhere else.
 */
static void
on_tick(lv_timer_t *timer)
{
    struct model m;

    (void)timer;
    model_get(&m);
    dashboard_update(&m, time(NULL));
}


/*
 * The name of the installation on the screen.
 *
 * The last segment of the topic prefix. MQTT_TOPIC_PREFIX is already
 * how a broker carrying more than one of these tells them apart --
 * `water-breaker/moses` -- so the name is there to be read rather than
 * configured a second time and kept in step by hand.
 *
 * Deliberately not the hostname. Over MQTT the machine running this is
 * whichever one someone opened a window on, which is not what the
 * screen is about: the first SDL build of it sat on a workstation
 * reporting `hyperion` above moses's water meter. The hostname becomes
 * the right answer again only for a source that is local by
 * construction -- the system bus of src/display/source-dbus.c, where
 * the producer is necessarily this machine.
 */
static const char *
device_name(const char *prefix)
{
    const char *slash = strrchr(prefix, '/');

    /* A prefix with no '/' is itself the name; one ending in '/' has no
     * last segment to take, so it is left whole rather than blank. */
    if ((slash == NULL) || (slash[1] == '\0'))
	return prefix;
    return slash + 1;
}


static void
display_parse_config(int argc, char **argv, struct display *d)
{
    static const char *const shortopts = "+i:s:u::S:ch";

    struct option longopts[] = {
	{ "interval",	required_argument,	NULL, 'i' },
	{ "stale",	required_argument,	NULL, 's' },
	{ "ups",	optional_argument,	NULL, 'u' },
	{ "source",	required_argument,	NULL, 'S' },
	{ "check",	no_argument,		NULL, 'c' },
	{ "help",	no_argument,		NULL, 'h' },
	{ 0 }
    };

    int opti, optc;

    for (;;) {
	optc = getopt_long(argc, argv, shortopts, longopts, &opti);
	if (optc < 0)
	    break;

	switch (optc) {
	case 'i':
	    if (parse_idle_timeout(optarg, &d->ups_interval) < 0)
		USAGE_DIE("invalid UPS polling interval (1s .. 10w)");
	    break;
	case 's':
	    if (parse_idle_timeout(optarg, &d->stale_after) < 0)
		USAGE_DIE("invalid staleness timeout (1s .. 10w)");
	    break;
	case 'u':
	    /* Three cases, and they are three different things: no --ups
	     * at all leaves use_upsd false and the UPS comes from the
	     * MQTT events; --ups with no name polls upsd and takes
	     * whichever UPS it lists first; --ups=NAME polls upsd for
	     * that one. */
	    d->use_upsd = true;
	    if (optarg != NULL) {
		d->ups = optarg;
	    } else if ((optind < argc) && (argv[optind][0] != '-')) {
		/* getopt only accepts an optional argument written up
		 * against its option (-umoses, --ups=moses), so a plain
		 * `-u moses` would otherwise leave the name behind as a
		 * stray operand. This program takes no operands, so
		 * picking it up here is unambiguous and is what anyone
		 * typing it expects. */
		d->ups = argv[optind++];
	    }
	    break;
	case 'S':
	    if      (strcmp(optarg, "auto") == 0)
		d->source = SOURCE_AUTO;
	    else if (strcmp(optarg, "mqtt") == 0)
		d->source = SOURCE_MQTT;
	    else if (strcmp(optarg, "dbus") == 0)
#ifdef WITH_DBUS
		d->source = SOURCE_DBUS;
#else
		USAGE_DIE("--source=dbus wants a build with WITH_DBUS");
#endif
	    else
		USAGE_DIE("invalid source (auto, mqtt, dbus)");
	    break;
	case 'c':
	    d->check_only = true;
	    break;
	case 'h':
	    printf("%s [opts]\n", __progname);
	    printf("  -i, --interval=SEC        poll upsd every SEC (default %d)\n",
		   UPS_INTERVAL);
	    printf("  -s, --stale=SEC           grey out a reading older than SEC (default %d)\n",
		   STALE_AFTER);
	    printf("  -u, --ups[=NAME]          poll upsd here, for charge and remaining time;\n");
	    printf("                            without NAME, the first UPS upsd lists\n");
	    printf("                            (omit -u entirely: UPS state from MQTT events)\n");
	    printf("  -S, --source=WHERE        the daemons' readings: mqtt, dbus, or auto\n");
	    printf("                            (default: dbus if it may own moses.display)\n");
	    printf("  -c, --check               bring the panel up, report, and exit;\n");
	    printf("                            the same checks a normal start makes\n");
	    printf("\n");
	    exit(0);
	default:
	    exit(1);
	}
    }
}


int
main(int argc, char *argv[])
{
    struct display *d = &display;

    __progname = basename(argv[0]);

    /* Configuration */
    mqtt_config_from_env(&d->mqtt);
    display_parse_config(argc, argv, d);

    model_init();

    /* The panel first: it is the one thing without which this program
     * has nothing to do, and failing before a socket is opened keeps
     * the failure easy to read. */
    if (backend_init() < 0)
	DIE(2, "failed to bring up the display");

    const struct backend_info *info = backend_info();
    LOG("Panel                : %s, %" LV_PRIu32 "x%" LV_PRIu32,
	info->name, info->hor_res, info->ver_res);
    (void)info;				/* LOG() is nothing without WITH_LOG */

    /* --check wanted the panel brought up and nothing else. Everything
     * that can be wrong about the wiring, the pins and the SPI device has
     * already said so by here, in the same words a normal start uses --
     * this option adds no check of its own, it only stops. */
    if (d->check_only) {
	backend_deinit();
	return 0;
    }

    backend_set_backlight(true);
    dashboard_create(backend_accent_color(), d->stale_after,
		     device_name(mqtt_topic_prefix()));

    /*
     * Where the UPS comes from is --ups, and nothing else.
     *
     * Named: upsd on this machine is asked about it, which is the only
     * way to get a charge and a remaining time. It is asked once here,
     * before anything starts, so the panel has a figure on it
     * immediately rather than one interval from now.
     *
     * Not named: upsd is not touched at all, and the UPS is whatever
     * the nut-notify events on the broker say it is -- a status, and no
     * figures. That is the right way round for a display that is not on
     * the machine the UPS is attached to, where localhost has no upsd
     * to ask and polling it would only ever report that there is no UPS
     * while the broker was carrying its events all along.
     */
    if (d->use_upsd) {
	source_nut_probe(d->ups);
	if (source_nut_start(d->ups, d->ups_interval) < 0)
	    DIE(2, "failed to start polling upsd");
    } else {
	LOG("UPS                  : from MQTT events (no --ups given)");
    }

    /* The events are watched either way. With upsd in play they name
     * the UPS it settled on -- given or discovered -- so a second UPS
     * on the broker cannot write over it; without, they are wildcarded
     * and are what the UPS state is made of. */
    /*
     * The daemons' readings come from one place, which --source picks.
     *
     * The bus is tried first unless the broker was asked for, and it
     * settles auto by owning moses.display: the bus reports no policy,
     * it only applies it, and dbus/moses.conf is what lets that name be
     * owned. Not by whether a daemon is on the bus right now -- at boot
     * this may well start before they do, and a bus without the policy
     * never carries anything however long it is watched.
     *
     * The bus, once chosen, is fatal to be without: a display that
     * looked like it was reading it and was not would be worse. Another
     * moses_display holding the name is fatal either way -- two would
     * fight over the panel -- and a refused name, asked for by hand, is
     * logged and read through regardless.
     */
    enum source source = d->source;
    const char  *why    = NULL;
#ifdef WITH_DBUS
    if (source != SOURCE_MQTT) {
	enum source_dbus_name name;

	if (source_dbus_start(&name) < 0) {
	    if (source == SOURCE_DBUS)
		DIE(2, "failed to reach the system bus");
	    source = SOURCE_MQTT;
	    why    = "no system bus";
	} else if (name == SOURCE_DBUS_NAME_TAKEN) {
	    DIE(2, "another moses_display owns moses.display");
	} else if (name == SOURCE_DBUS_NAME_OWNED) {
	    if (source == SOURCE_AUTO)
		why = "owns moses.display";
	    source = SOURCE_DBUS;
	} else if (source == SOURCE_AUTO) {
	    source_dbus_stop();
	    source = SOURCE_MQTT;
	    why    = "moses.display refused, no bus policy";
	}
    }
#endif
    if (source == SOURCE_AUTO)		/* built without the bus */
	source = SOURCE_MQTT;
    LOG("Source               : %s%s%s%s",
	(source == SOURCE_DBUS) ? "dbus" : "mqtt",
	why ? " (auto: " : "", why ? why : "", why ? ")" : "");
    (void)why;

    /* The broker, for the readings when it is their source, and for
     * nut-notify's UPS events either way -- they are only ever on the
     * broker. With upsd in play they name the UPS it settled on --
     * given or discovered -- so a second UPS on the broker cannot write
     * over it; without, they are wildcarded and are what the UPS state
     * is made of.
     *
     * Fatal only when the broker is where the readings come from. With
     * the bus as their source, a broker that cannot be reached costs
     * the UPS events and nothing else, and dying over it would take the
     * panel down in the one situation the bus is there for: the network
     * being down when this starts. */
    if (source_mqtt_start(&d->mqtt, source == SOURCE_MQTT,
			  d->use_upsd ? source_nut_name() : NULL,
			  ! d->use_upsd) < 0) {
	if (source == SOURCE_MQTT)
	    DIE(2, "failed to subscribe to MQTT");
	LOG("MQTT unreachable, carrying on without the UPS events");
    }


    /* Unlike the daemons this one does clean up: the backlight is a
     * lit panel left behind, not a valve, so there is something worth
     * turning off and no last will to suppress by doing it. */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    lv_timer_create(on_tick, UPDATE_PERIOD_MS, NULL);
    on_tick(NULL);			/* draw what is known now */

    while (running) {
	uint32_t idle = lv_timer_handler();
	if (idle == LV_NO_TIMER_READY)
	    idle = LV_DEF_REFR_PERIOD;
	usleep(idle * 1000);
    }

#ifdef WITH_DBUS
    source_dbus_stop();			/* nothing when it was not started */
#endif
    backend_deinit();
    return EXIT_SUCCESS;
}
