/*
 * moses_watermeter -- read the water meter and report consumption.
 *
 * Two independent and optional data sources run as separate threads:
 *
 *   - index_reader     Periodically queries the absolute meter index
 *                      (total volume in litres) over M-Bus, using libmbus.
 *                      Published on the `index` topic every --interval
 *                      seconds.
 *
 *   - pulse_counting   Watches a GPIO line wired to the meter pulse
 *                      output (e.g. Sensus HRI) and counts edge events
 *                      via the Linux GPIO character device (uapi v2).
 *                      The pulse count is published on the `pulse` topic.
 *                      With --idle-timeout a `0` is published when no
 *                      pulse is seen within the timeout, giving a
 *                      regular heartbeat.
 *
 * Either source may be left unconfigured; only the configured ones are
 * started. Read failures are reported on the `error` topic. All topics
 * are relative to MQTT_TOPIC_PREFIX (see common.c).
 *
 * With --leak (or any --leak-*), the litres also go through the leak
 * signatures of src/leak.c, from one source chosen by --leak-source:
 * the index, the pulses, or -- auto, the default -- the pulses while
 * they agree with the index and the index otherwise (leak_from_index()).
 * The report is published, retained, on the
 * `leak` topic whenever it changes. Reported only: closing the valve is
 * someone else's decision (DESIGN.md, *Leaks are reported, not acted
 * on*).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include <poll.h>
#include <pthread.h>

#include <string.h>
#include <errno.h>
#include <linux/gpio.h>
#include <time.h>

#include <getopt.h>
#include <libgen.h>

#include <mbus/mbus.h>

#include "common.h"
#include "leak.h"

//== Constants =========================================================

/* Inferred from: uapi/linux/gpio.h */
#define MAX_EVENTS ((GPIO_V2_LINES_MAX) * 16)



//== Structures ========================================================

struct pulse_counting {
    struct {                     // Controller
	char    *id;             //  - identifier
	int      fd;             //  - file descriptor
    } ctrl;
    struct {                     // Pin
	uint32_t id;             //  - identifier
	int      fd;             //  - file descriptor
	uint64_t flags;          //  - flags
	char    *label;          //  - label
    } pin;
    struct {                     // Flags
	uint8_t debounce    :1;  //   - debounce
	uint8_t idle_timeout:1;  //   - idle timeout
    } flags;
    uint32_t debounce;           // debounce time in µs
    unsigned long idle_timeout;  // idle timeout in s
};

struct index_reader {
    char         *device;         // serial device
    long          baudrate;       // baudrate
    char         *address;        // primary or secondary address
    mbus_handle  *mbus;           // mbus
    unsigned int  count;          // call counting
    unsigned long interval;
};

struct watermeter_mqtt {          // MQTT
    struct mqtt handler;
    struct {
	char *pulse;
	char *index;
	char *error;
	char *avail;
	char *leak;
    } topic;
};

enum leak_source { LEAK_SOURCE_AUTO = 0, LEAK_SOURCE_INDEX, LEAK_SOURCE_PULSE };

struct leak_watch {
    bool               enabled;
    enum leak_source   source;
    struct leak_config cfg;
    struct leak        state;
    struct pulse_check check;        // pulses against the index (auto)
    unsigned long      pulses;       // counted since the last index reading
    pthread_mutex_t    lock;         // all of the above
    double             index;        // last index read, < 0 before one
    double             carry;        // index fraction not yet a litre
};

struct watermeter {
    struct watermeter_mqtt mqtt;
    struct pulse_counting  pulse_counting;
    struct index_reader    index_reader;
    struct leak_watch      leak;
    int                    reduced_latency;
};


//== Global context ====================================================

char *__progname = "??";

struct watermeter watermeter =  {
    .mqtt     = {
	.handler     = MQTT_INITIALIZER(),
	.topic.pulse = "pulse",
	.topic.index = "index",
	.topic.error = "error",
	.topic.avail = "availability/watermeter",
	.topic.leak  = "leak",
    },
    .pulse_counting = {
	.ctrl.id   = NULL,
	.ctrl.fd   = -1,
	.pin.id    = ~0,
	.pin.fd    = -1,
	.pin.flags = GPIO_V2_LINE_FLAG_EDGE_RISING,
	.pin.label = "pulse-counting",
    },
    .index_reader    = {
	.device    = "/dev/ttyAMA0",
	.baudrate  = 2400,
	.address   = "1",
	.interval  = 60,
    },
    .leak = {
	.cfg   = { .flow = 20 * 60, .slow = 8, .quiet = 2 * 3600 },
	.lock  = PTHREAD_MUTEX_INITIALIZER,
	.index = -1,
    },
};



//== m-bus =============================================================

mbus_handle *
mbus_open(char *device, long baudrate)
{
    mbus_handle *h = NULL;

    if (baudrate < 0)
	baudrate = 2400;

    if ((h = mbus_context_serial(device)) == NULL)
	goto failed_serial;
    if (mbus_connect(h) == -1)
	goto failed_connect;
    if (mbus_serial_set_baudrate(h, baudrate) == -1)
	goto failed_baudrate;

    return h;
    
 failed_baudrate:
    mbus_disconnect(h);
 failed_connect:
    mbus_context_free(h);
 failed_serial:
    return NULL;
}

int
mbus_close(mbus_handle *h) {
    if (h != NULL) {
	mbus_disconnect(h);
	mbus_context_free(h);
    }
    return 0;
}

int
mbus_softreset(mbus_handle *h)
{
    // Initialise slaves
    if ((mbus_send_ping_frame(h, MBUS_ADDRESS_NETWORK_LAYER, 1) == -1) ||
	(mbus_send_ping_frame(h, MBUS_ADDRESS_NETWORK_LAYER, 1) == -1))
	return -1;
    return 0;
}



int
mbus_watermeter_get_index(mbus_handle *h, char *addr, double *index)
{
    mbus_frame         reply      = { 0 };
    mbus_frame_data    reply_data = { 0 };
    int                address    = -1;
    
    // Select device
    if (mbus_is_secondary_address(addr)) {
	if (mbus_select_secondary_address(h, addr) != MBUS_PROBE_SINGLE)
	    return -1;
        address = MBUS_ADDRESS_NETWORK_LAYER;
    } else {
        address = atoi(addr);
    }
    
    // Perform query
    if ((mbus_send_request_frame(h, address)        == -1                 ) ||
	(mbus_recv_frame(h, &reply)                 != MBUS_RECV_RESULT_OK) ||
	(mbus_frame_data_parse(&reply, &reply_data) == -1                 ))
	return -1;

    // Explicit error?
    if (reply_data.type == MBUS_DATA_TYPE_ERROR)
	return -1;

    // Unhandled type?
    if (reply_data.type != MBUS_DATA_TYPE_VARIABLE)
	return -1;
	
    mbus_data_variable        *data   = &reply_data.data_var;
    int                        rc     = -1;

#if 0
    mbus_data_variable_header *header = &data->header;
    char manufacturer_id[sizeof("XYZ-12345678")] = { 0 };
    snprintf(manufacturer_id, sizeof(manufacturer_id), "%s-%08x",
	     mbus_decode_manufacturer(header->manufacturer[0],
				      header->manufacturer[1]),
	     mbus_data_bcd_decode_hex(header->id_bcd, 4));
#endif
    
    for (mbus_data_record *r = data->record ; r ; r = r->next ) {
	double v_real;
	char  *v_str;
	int    v_strlen;
	if (mbus_variable_value_decode(r, &v_real, &v_str, &v_strlen) < 0) {
	    goto cleanup;
	}

	switch(r->drh.vib.vif) {
	case 0x10: *index = v_real *     0.001; goto found;
	case 0x11: *index = v_real *     0.01;  goto found;
	case 0x12: *index = v_real *     0.1;   goto found;
	case 0x13: *index = v_real *     1.0;   goto found;
	case 0x14: *index = v_real *    10.0;   goto found;
	case 0x15: *index = v_real *   100.0;   goto found;
	case 0x16: *index = v_real *  1000.0;   goto found;
	case 0x17: *index = v_real * 10000.0;   goto found;
	case 0x78: /* serial */                 break;
	}

	// mbus_data_variable_print(data);
    }
    goto cleanup;
    
 found:
    rc = 0;
    
 cleanup:
    // Free records
    if (data->record)
        mbus_data_record_free(data->record);

    return rc;
}



//== MQTT ==============================================================


int
watermeter_mqtt_init(struct watermeter_mqtt *mqtt)
{
    // Adjust prefix
    const char *prefix = mqtt_topic_prefix();
    MQTT_ADJUST_TOPIC(mqtt, pulse, prefix);
    MQTT_ADJUST_TOPIC(mqtt, index, prefix);
    MQTT_ADJUST_TOPIC(mqtt, error, prefix);
    MQTT_ADJUST_TOPIC(mqtt, avail, prefix);
    MQTT_ADJUST_TOPIC(mqtt, leak,  prefix);

    if (mqtt_enabled(&mqtt->handler)) {
	LOG("MQTT pulse           : %s", mqtt->topic.pulse);
	LOG("MQTT index           : %s", mqtt->topic.index);
	LOG("MQTT error reporting : %s", mqtt->topic.error);
	LOG("MQTT availability    : %s", mqtt->topic.avail);
	if (watermeter.leak.enabled)
	    LOG("MQTT leak            : %s", mqtt->topic.leak);
    }

    int rc = mqtt_connect(&mqtt->handler, 0, NULL, mqtt->topic.avail, NULL);
    if (rc < 0) return -1;
    if (rc > 0) LOG("MQTT connection      : established");

    return 0;
}



//======================================================================

int
watermeter_init(struct watermeter *w)
{
    struct watermeter_mqtt *mqtt = &w->mqtt;
    struct pulse_counting  *pc   = &w->pulse_counting;
    struct index_reader    *ir   = &w->index_reader;
    
    if (watermeter_mqtt_init(mqtt) < 0)
	return -1;
    
    
    //
    // M-BUS
    //
    if (ir->device != NULL) {
	ir->mbus = mbus_open(ir->device, ir->baudrate);
	if (ir->mbus == NULL) {
	    LOG("failed to open/connect to m-bus (dev=%s, baudrate=%ld)",
		ir->device, ir->baudrate);
	    goto failed_mbus;
	}
	mbus_softreset(ir->mbus);
	LOG("M-Bus device         : %s at %ld bauds", ir->device, ir->baudrate);
    } else {
	LOG("M-Bus device         : none (skipping)");
    }
    
    
    //
    // GPIO
    //
    if ((pc->ctrl.id != NULL) || (pc->pin.id != ~0U)) {
	// Single input line, with optional hardware debounce.
	//  (attrs slots beyond num_attrs must stay zeroed:
	//   recent kernels reject requests with data in unused slots)
	struct gpio_v2_line_request req = {
	    .config.flags     = GPIO_V2_LINE_FLAG_INPUT | pc->pin.flags,
	};
	if (pc->flags.debounce) {
	    req.config.num_attrs                        = 1;
	    req.config.attrs[0].mask                    = 1 << 0;
	    req.config.attrs[0].attr.id                 = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
	    req.config.attrs[0].attr.debounce_period_us = pc->debounce;
	}

	int ctrl_fd = gpio_open_line(pc->ctrl.id, pc->pin.id,
				     pc->pin.label, &req);
	if (ctrl_fd < 0)
	    goto failed_gpio;

	pc->ctrl.fd = ctrl_fd;
	pc->pin.fd  = req.fd;
    } else {
	LOG("GPIO line            : none (skipping)");
    }

    return 0;

 failed_gpio:
    if (pc->ctrl.fd >= 0) close(pc->ctrl.fd);
    if (pc->pin.fd  >= 0) close(pc->pin.fd );
    pc->ctrl.fd = -1;
    pc->pin.fd  = -1;
    
 failed_mbus:
    mbus_close(ir->mbus);
    return -1;   
}


int
watermeter_get_index(struct watermeter *w, double *index)
{
    struct index_reader *ir = &w->index_reader;

    int retries = 1;
 retry:
    if (mbus_watermeter_get_index(ir->mbus, ir->address, index) < 0) {
	if (mbus_softreset(ir->mbus) < 0)
	    return -1;
	if (retries-- > 0) goto retry;
	else               return -1;
    }
    return 0;
}



//== Leak ==============================================================

static double
monotonic_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool leak_from_index(const struct watermeter *w);

// Publish the current report: retained on `leak`, and as a line.
// Called with the lock held.
static void
leak_publish(struct watermeter *w)
{
    const struct leak_report *r = leak_report(&w->leak.state);

    // `since` is monotonic inside src/leak.c; the world wants a date.
    long long since = 0;
    if (r->level != LEAK_OK) {
	struct timespec rt;
	clock_gettime(CLOCK_REALTIME, &rt);
	since = (long long)(rt.tv_sec - (monotonic_now() - r->since));
    }

    const char *source = leak_from_index(w) ? "index" : "pulse";
    PUT_DATA("watermeter", "leak=%d,kind=\"%s\",since=%lld,litres=%lu,rate=%.2f,"
	     "source=\"%s\"", (int)r->level, leak_kind_name(r->kind), since,
	     r->litres, r->rate, source);
    MQTT_PUBLISH(&w->mqtt, leak, 1, true,
		 "{ \"level\": \"%s\", \"kind\": \"%s\", \"since\": %lld, "
		 "\"litres\": %lu, \"rate\": %.2f, \"source\": \"%s\" }",
		 leak_level_name(r->level), leak_kind_name(r->kind), since,
		 r->litres, r->rate, source);
    LOG("leak                 : %s %s", leak_level_name(r->level),
	leak_kind_name(r->kind));
}

static bool
has_index(const struct watermeter *w)
{
    return w->index_reader.device != NULL;
}

static bool
has_pulses(const struct watermeter *w)
{
    return w->pulse_counting.ctrl.id != NULL;
}

// Which source the leak rules read, one at a time so no litre counts
// twice. `index` and `pulse` are what they say. `auto` takes the only
// one configured, and with both, the pulses while the index vouches for
// them (struct pulse_check) -- they carry each litre's own time -- and
// the index until then and whenever they stop agreeing: a -P pin says
// nothing about the wiring behind it. Called with the lock held.
static bool
leak_from_index(const struct watermeter *w)
{
    switch (w->leak.source) {
    case LEAK_SOURCE_INDEX: return true;
    case LEAK_SOURCE_PULSE: return false;
    case LEAK_SOURCE_AUTO:  break;
    }
    if (!has_pulses(w)) return true;
    if (!has_index(w))  return false;
    return w->leak.check.health != PULSE_OK;
}

// Pulses as read off the line, each a litre (docs/hardware.md, *Pulse
// counting*) at the kernel's own timestamp -- CLOCK_MONOTONIC unless
// asked otherwise, which this daemon never does. Counted for the
// check, fed to the rules only when they are the source.
static void
leak_feed_pulses(struct watermeter *w,
		 const struct gpio_v2_line_event *ev, int n)
{
    if (!w->leak.enabled || (n <= 0))
	return;
    pthread_mutex_lock(&w->leak.lock);
    w->leak.pulses += n;
    if (!leak_from_index(w)) {
	bool changed = false;
	for (int i = 0 ; i < n ; i++)
	    changed |= leak_litres(&w->leak.state, ev[i].timestamp_ns / 1e9, 1);
	if (changed)
	    leak_publish(w);
    }
    pthread_mutex_unlock(&w->leak.lock);
}

// The pulses changed health: say so where someone will see it.
static void
pulse_health_changed(struct watermeter *w)
{
    const struct pulse_check *c = &w->leak.check;
    if (c->health == PULSE_BROKEN) {
	LOG("pulses               : broken, %s; leak rules on the index", c->why);
	PUT_FAIL("watermeter", "pulse-check");
	MQTT_PUBLISH(&w->mqtt, error, 1, false,
		     "{ \"source\": \"watermeter\", \"type\": \"pulse\", "
		     "\"msg\": \"pulses do not match the index: %s\" }", c->why);
    } else if (c->health == PULSE_OK) {
	LOG("pulses               : agree with the index (%s); leak rules on the pulses",
	    c->why);
    }
    leak_publish(w);                       // `source` moved
}

// An index reading. What it grew by, in whole litres, goes to the rules
// when the index is the source -- decided before the check below, so a
// switch never drops nor doubles a reading -- and, with both sources
// configured under auto, to the pulse check. A backwards or implausible
// step (meter swapped, bad frame) resets the reference rather than
// counting, and starts the pulse window over.
static void
leak_index_reading(struct watermeter *w, double t, double index)
{
    struct leak_watch *lw = &w->leak;
    if (!lw->enabled)
	return;
    pthread_mutex_lock(&lw->lock);

    if ((lw->index < 0) || (index < lw->index) || (index - lw->index >= 1000)) {
	lw->index  = index;
	lw->carry  = 0;
	lw->pulses = 0;
	pthread_mutex_unlock(&lw->lock);
	return;
    }
    lw->carry += index - lw->index;
    lw->index  = index;
    unsigned n = (unsigned)lw->carry;
    lw->carry -= n;

    if (leak_from_index(w) && (n > 0) && leak_litres(&lw->state, t, n))
	leak_publish(w);

    if ((lw->source == LEAK_SOURCE_AUTO) && has_pulses(w)) {
	unsigned long p = lw->pulses;
	lw->pulses = 0;
	if (pulse_check_reading(&lw->check, n, (unsigned)p))
	    pulse_health_changed(w);
    }
    pthread_mutex_unlock(&lw->lock);
}

static double
parse_leak_duration(const char *arg)
{
    if (strcmp(arg, "0") == 0)
	return 0;
    unsigned long v;
    if (parse_idle_timeout(arg, &v) < 0)
	USAGE_DIE("invalid leak duration (0 to disable, or 1s .. 10w)");
    return (double)v;
}

//======================================================================


static void
watermeter_parse_config(int argc, char **argv, struct watermeter *w)
{
    struct pulse_counting *pc = &w->pulse_counting;
    struct index_reader   *ir = &w->index_reader;

    static const char *const shortopts = "+rd:b:a:i:P:L:D:B:E:I:h";
    
    const struct option longopts[] = {
	{ "reduced-latency", no_argument,	NULL,	'r' },
	{ "device",          required_argument, NULL,   'd' },
	{ "baudrate",        required_argument, NULL,   'b' },
	{ "address",         required_argument, NULL,   'a' },
	{ "interval",        required_argument, NULL,   'i' },
	{ "pin",             required_argument, NULL,	'P' },
	{ "pin-label",       required_argument, NULL,	'L' },
	{ "debounce",        required_argument, NULL,	'D' },
	{ "bias",            required_argument, NULL,	'B' },
	{ "edge",            required_argument, NULL,	'E' },
	{ "idle-timeout",    required_argument, NULL,	'I' },
	{ "leak",            no_argument,       NULL,   0x100 },
	{ "leak-flow",       required_argument, NULL,   0x101 },
	{ "leak-slow",       required_argument, NULL,   0x102 },
	{ "leak-quiet",      required_argument, NULL,   0x103 },
	{ "leak-source",     required_argument, NULL,   0x104 },
	{ "help",	     no_argument,	NULL,	'h' },
	{ NULL },
    };

    int opti, optc;
    
    for (;;) {
	optc = getopt_long(argc, argv, shortopts, longopts, &opti);
	if (optc < 0)
	    break;
	
	switch (optc) {
	case 'r':
	    w->reduced_latency = 1;
	    break;
	case 'd':
	    ir->device = optarg;
	    break;
	case 'b':
	    if (parse_mbus_baudrate(optarg, &ir->baudrate) < 0)
		USAGE_DIE("invalid baud rate"
			  " (300, 600, 1200, 2400, 4800, 9600, 19200, 38400)");
	    break;
	case 'a':
	    ir->address = optarg;
	    break;
	case 'i':
	    if (parse_idle_timeout(optarg, &ir->interval) < 0)
		USAGE_DIE("invalid reporting interval (1s .. 10w)");
	    break;
	case 'P':
	    if (parse_gpio(optarg, &pc->ctrl.id, &pc->pin.id) < 0)
		USAGE_DIE("invalid GPIO pin (chipset:pin)");
	    break;
	case 'L':
	    pc->pin.label = optarg;
	    break;
	case 'D':
	    if (parse_gpio_debounce(optarg, &pc->debounce) < 0)
		USAGE_DIE("invalid debounce time (1us .. 1h)");
	    pc->flags.debounce = 1;
	    break;
	case 'E':
	    if (parse_gpio_edge(optarg, &pc->pin.flags) < 0)
		USAGE_DIE("invalid edge (rising, failing)");
	    break;
	case 'B':
	    if (parse_gpio_bias(optarg, &pc->pin.flags) < 0)
		USAGE_DIE("invalid bias (as-is, disabled, pull-up, pull-down)");
	    break;
	case 'I':
	    if (parse_idle_timeout(optarg, &pc->idle_timeout) < 0)
		USAGE_DIE("invalid idle timeout (1s .. 10w)");
	    pc->flags.idle_timeout = 1;
	    break;
	case 0x100:
	    w->leak.enabled = true;
	    break;
	case 0x101:
	    w->leak.cfg.flow  = parse_leak_duration(optarg);
	    w->leak.enabled   = true;
	    break;
	case 0x102: {
	    char *end = NULL;
	    unsigned long v = strtoul(optarg, &end, 10);
	    if ((end == optarg) || (*end != '\0') ||
		((v != 0) && ((v < 3) || (v > LEAK_SLOW_MAX))))
		USAGE_DIE("invalid leak count (0 to disable, or 3 .. %d)",
			  LEAK_SLOW_MAX);
	    w->leak.cfg.slow = (unsigned)v;
	    w->leak.enabled  = true;
	    break;
	}
	case 0x103:
	    w->leak.cfg.quiet = parse_leak_duration(optarg);
	    w->leak.enabled   = true;
	    break;
	case 0x104:
	    if      (strcmp(optarg, "auto")  == 0) w->leak.source = LEAK_SOURCE_AUTO;
	    else if (strcmp(optarg, "index") == 0) w->leak.source = LEAK_SOURCE_INDEX;
	    else if (strcmp(optarg, "pulse") == 0) w->leak.source = LEAK_SOURCE_PULSE;
	    else USAGE_DIE("invalid leak source (auto, index, pulse)");
	    w->leak.enabled = true;
	    break;
	case 'h':
	    printf("pulse-counting [opts]\n");
	    printf("  -r, --reduced-latency            try to reduce latency\n");
	    printf("  -d, --device=DEV                 m-bus serial device\n");
	    printf("  -b, --baudrate=BAUDS             m-bus baudrate\n");
	    printf("  -a, --address=ADDR               m-bus primary or secondary\n");
	    printf("  -i, --interval=SEC               reporting index interval\n");
	    printf("  -P, --pin=CTRL:PIN               gpio pulse counting pin\n");
	    printf("  -L, --pin-label=STRING           gpio pin label\n");
	    printf("  -D, --debounce=USEC              gpio debouncing\n");
	    printf("  -B, --bias=as-is|disabled|       gpio bias\n");
	    printf("             pull-up|pull-down\n");
	    printf("  -E, --edge=rising|falling        gpio edge detection\n");
	    printf("  -I, --idle-timeout=SEC           gpio notify if no pulse\n");
	    printf("      --leak                       leak signatures, defaults below\n");
	    printf("      --leak-flow=SEC              uninterrupted flow (20min, 0 off)\n");
	    printf("      --leak-slow=COUNT            evenly spaced lone litres (8, 0 off)\n");
	    printf("      --leak-quiet=SEC             quiet expected per 24h (2h, 0 off)\n");
	    printf("      --leak-source=auto|index|pulse  litres from (auto)\n");
	    printf("\n");
	    exit(0);
	case 0:
	    break;
	default:
	    exit(1);
	}
    }
    argc -= optind;
    argv += optind;

    if (w->leak.enabled) {
	if ((w->leak.source == LEAK_SOURCE_INDEX) && !has_index(w))
	    USAGE_DIE("--leak-source=index wants an M-Bus device");
	if ((w->leak.source == LEAK_SOURCE_PULSE) && !has_pulses(w))
	    USAGE_DIE("--leak-source=pulse wants a pulse pin (-P)");
	if (!has_index(w) && !has_pulses(w))
	    USAGE_DIE("--leak wants the index or the pulses");
    }
}



//======================================================================

static pthread_t thr_pulse_counting;
static pthread_t thr_index_reader;
static pthread_t thr_leak_ticker;


__attribute__((noreturn))
static void * index_reader_task(void *parameters) {
    struct watermeter_mqtt *mqtt = &watermeter.mqtt;
    struct index_reader    *ir   = parameters;

    // Polling
    struct timespec next_polling;
    clock_gettime(INTERVAL_CLOCK, &next_polling);
    while (1) {
	// Watermeter
	double value;
	if (watermeter_get_index(&watermeter, &value) < 0) {
	    PUT_FAIL("watermeter", "read");
	    static char *msg =
		MQTT_ERROR_MSG("watermeter", "error",
			       "failed to read index");
	    MQTT_PUBLISH(mqtt, error, 1, false, "%s", msg);
	} else {
	    PUT_DATA("watermeter", "index=%0.3f", value);
	    MQTT_PUBLISH(mqtt, index, 1, false, "%0.3f", value);
	    leak_index_reading(&watermeter, monotonic_now(), value);
	}
	
	// Next
	next_polling.tv_sec += ir->interval;
	sleep_until(INTERVAL_CLOCK, &next_polling);
    }
}


__attribute__((noreturn))
static void * pulse_counting_task(void *parameters) {
    struct watermeter_mqtt *mqtt = &watermeter.mqtt;
    struct pulse_counting  *pc   = parameters;

    while(1) {
	int pulse = 0;

	if (pc->flags.idle_timeout) {
	    struct timespec ts = {
		.tv_sec  = pc->idle_timeout
	    };
	    struct pollfd pfd = {
		.fd     = pc->pin.fd,
		.events = POLLIN | POLLPRI
	    };
	    int rc = ppoll(&pfd, 1, &ts, NULL);
	    if (rc < 0) {
		LOG_ERRNO("ppoll failed");
		continue;
	    } else if (rc == 0) {
		goto publish;
	    }
	}

	struct gpio_v2_line_event event[MAX_EVENTS];
	ssize_t size = read(pc->pin.fd, event, sizeof(event));
	    
	if (size < 0) {
	    LOG_ERRNO("failed to read event");
	    PUT_FAIL("watermeter", "pulse");

	    static char *msg =
		MQTT_ERROR_MSG("watermeter", "error",
			       "failed to read pulse");
	    MQTT_PUBLISH(mqtt, error, 1, false, "%s", msg);
	    continue;
	} else if (size % sizeof(struct gpio_v2_line_event)) {
	    LOG("got event of unexpected size");
	    continue;
	}

	pulse = size / sizeof(struct gpio_v2_line_event);

	leak_feed_pulses(&watermeter, event, pulse);
	
    publish:
	PUT_DATA("watermeter", "pulse=%d", pulse);
	MQTT_PUBLISH(mqtt, pulse, 2, false, "%u", pulse);
    }
}


__attribute__((noreturn))
static void * leak_ticker_task(void *parameters) {
    struct watermeter *w = parameters;
    while (1) {
	sleep(30);
	pthread_mutex_lock(&w->leak.lock);
	if (leak_tick(&w->leak.state, monotonic_now()))
	    leak_publish(w);
	pthread_mutex_unlock(&w->leak.lock);
    }
}


int
main(int argc, char **argv)
{
    __progname = basename(argv[0]);

    // Configuration
    mqtt_config_from_env(&watermeter.mqtt.handler);
    watermeter_parse_config(argc, argv, &watermeter);

    // One at a time (common.c, single_instance)
    pid_t other = single_instance("watermeter");
    if (other > 0)
	DIE(2, "already running as pid %d", (int)other);
    if (other < 0)
	DIE(2, "already running");

    LOG_REDUCED_LATENCY(watermeter.reduced_latency);

    // Initialization
    if (watermeter_init(&watermeter) < 0)
	DIE(2, "failed to initialize");

    // On the system bus, when built with it (src/dbus_sink.h). Never
    // fatal, and before reduced_latency() so its thread is an ordinary
    // one rather than SCHED_FIFO.
    DBUS_SINK_START("watermeter");

    // Reducing latency
    if (watermeter.reduced_latency)
	reduced_latency();

    // Leak signatures: start clean, and say so -- the topic is
    // retained, and a report from before a restart is not evidence now.
    if (watermeter.leak.enabled) {
	struct leak_config *c = &watermeter.leak.cfg;
	static const char *const choice[] = { "auto", "index", "pulse" };
	pulse_check_init(&watermeter.leak.check);
	LOG("Leak rules           : flow %.0fs, slow %u, quiet %.0fs",
	    c->flow, c->slow, c->quiet);
	LOG("Leak source          : %s, %s for now", choice[watermeter.leak.source],
	    leak_from_index(&watermeter) ? "index" : "pulses");
	(void)choice;                      // LOG may be compiled out
	leak_init(&watermeter.leak.state, c, monotonic_now());
	pthread_mutex_lock(&watermeter.leak.lock);
	leak_publish(&watermeter);
	pthread_mutex_unlock(&watermeter.leak.lock);
    }

    // Starting threads
    if (watermeter.leak.enabled)
	pthread_create(&thr_leak_ticker, NULL,
		       leak_ticker_task, &watermeter);
    if (watermeter.pulse_counting.ctrl.id)
	pthread_create(&thr_pulse_counting, NULL,
		       pulse_counting_task, &watermeter.pulse_counting);
    if (watermeter.index_reader.device)
	pthread_create(&thr_index_reader,   NULL,
		       index_reader_task,   &watermeter.index_reader);

    // Waiting... (they are not suppose to terminate)
    if (watermeter.pulse_counting.ctrl.id)
	pthread_join(thr_pulse_counting, NULL);
    if (watermeter.index_reader.device)
	pthread_join(thr_index_reader,   NULL);
    
    
    return 0;
}



