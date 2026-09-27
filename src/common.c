/*
 * common.c -- code shared by the moses programs.
 *
 *   - Command-line option parsers (parse_*): baud rate and time periods.
 *   - reduced_latency(): switch to the SCHED_FIFO real-time scheduler
 *     and lock memory, to keep pulse counting / valve control responsive.
 *   - A thin MQTT wrapper around libmosquitto (mqtt_*): connection,
 *     automatic reconnection with re-subscription, printf-style publish,
 *     and configuration from the MQTT_* environment variables.
 *
 * Deliberately free of <linux/gpio.h>: everything that needs it lives in
 * gpio.c, so that this file -- and so moses_display -- builds on a
 * development machine too.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>

#ifdef WITH_MQTT
#include <mosquitto.h>
#endif

#include "common.h"


/************************************************************************
 * Parsers                                                              *
 ************************************************************************/

int
parse_mbus_baudrate(const char *option, long *val)
{
    char *end = NULL;
    long    v = strtoul(option, &end, 10);

    if ((*option == '\0') || (*end != '\0'))
	return -1;
    
    switch(v) {
    case 300:     case 600:     case 1200:     case 2400:
    case 4800:    case 9600:    case 19200:    case 38400:
	break;
    default:
	return -1;
    }

    *val = v;
    return 0;
}


int
parse_s_period(const char *option, uint64_t *val)
{
    _Static_assert(sizeof(long long) ==	sizeof(uint64_t));

    errno = 0;
    char    *end = NULL;
    uint64_t   v = strtoull(option, &end, 10);
    if ((v == ULLONG_MAX) && (errno != 0)) return -1;

    uint64_t mult;
    if      (*end == '\0'           ) { mult =      1ull; }
    else if (strcmp(end, "s"  ) == 0) { mult =      1ull; }
    else if (strcmp(end, "min") == 0) { mult =     60ull; }
    else if (strcmp(end, "h"  ) == 0) { mult =   3600ull; }
    else if (strcmp(end, "d"  ) == 0) { mult =  86400ull; }
    else if (strcmp(end, "w"  ) == 0) { mult = 604800ull; }
    else                              { return -1;        }

    // Overflow-checked multiply. __builtin_mul_overflow is GCC>=5 /
    // Clang>=3.4; switch to the C23 ckd_mul() from <stdckdint.h> once the
    // toolchain baseline (GCC 14+, Clang 18+) makes that header reliable.
    if (__builtin_mul_overflow(v, mult, &v)) return -1;

    *val = v;
    return 0;
}


int
parse_us_period(const char *option, uint64_t *val)
{
    _Static_assert(sizeof(long long) ==	sizeof(uint64_t));

    errno = 0;
    char    *end = NULL;
    uint64_t   v = strtoull(option, &end, 10);
    if ((v == ULLONG_MAX) && (errno != 0)) return -1;

    uint64_t mult;
    if      (*end == '\0'           ) { mult =          1ull; }
    else if (strcmp(end, "us" ) == 0) { mult =          1ull; }
    else if (strcmp(end, "ms" ) == 0) { mult =       1000ull; }
    else if (strcmp(end, "s"  ) == 0) { mult =    1000000ull; }
    else if (strcmp(end, "min") == 0) { mult =   60000000ull; }
    else if (strcmp(end, "h"  ) == 0) { mult = 3600000000ull; }
    else                              { return -1;            }

    // Overflow-checked multiply (see parse_s_period for the ckd_mul note).
    if (__builtin_mul_overflow(v, mult, &v)) return -1;

    *val = v;
    return 0;
}


// Return delay in seconds (range: 1 sec to 10 weeks)
int
parse_idle_timeout(const char *option, unsigned long *val)
{
    uint64_t v;
    if ((parse_s_period(option, &v) <  0          ) ||
	(v                          <= 0          ) ||
	(v                          >  UINT64_MAX ) ||
	(v                          >  6048000ull ))
	return -1;

    *val = v;
    return 0;
}




/************************************************************************
 * System tuning                                                        *
 ************************************************************************/

void
sleep_until(clockid_t clock, const struct timespec *deadline)
{
    // clock_nanosleep() returns 0 on success or a (positive) error number;
    // it does not set errno. EINTR means a signal cut the wait short, so
    // restart to honor the absolute deadline; anything else is a bug here.
    int rc;
    while ((rc = clock_nanosleep(clock, TIMER_ABSTIME, deadline, NULL)) != 0) {
	assert(rc == EINTR);
    }
}


void
reduced_latency(void)
{
    LOG("configuring for reduced latency");

    // Change scheduler priority to be more "real-time". Best-effort: this
    // needs CAP_SYS_NICE, so warn rather than abort when it is denied.
    struct sched_param sp = {
        .sched_priority = sched_get_priority_max(SCHED_FIFO),
    };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
        LOG_ERRNO("failed to switch to SCHED_FIFO scheduler");
    }

    // Avoid swapping by locking pages in memory. Needs CAP_IPC_LOCK.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
        LOG_ERRNO("failed to lock memory (mlockall)");
    }
}



/************************************************************************
 * Readings out                                                         *
 ************************************************************************/

#if defined(WITH_LINEPROTOCOL) || defined(WITH_DBUS)

/*
 * A reading leaves this program as one line of InfluxDB line protocol,
 * and goes to whichever sinks the build has: stdout, the system bus, or
 * both. See common.h for the shape of the line, and src/dbus_sink.h for
 * the bus -- a sink arranged so that it cannot hold a daemon up, because
 * a valve controller must never wait on a display.
 *
 * One formatter, so the two sinks cannot drift into carrying slightly
 * different renderings of the same reading.
 */

// Hand one finished line to every sink compiled in.
static void
put_line(const char *line, size_t len)
{
#ifdef WITH_LINEPROTOCOL
    fwrite(line, 1, len, stdout);
    fflush(stdout);
#endif
#ifdef WITH_DBUS
    /* Without the trailing newline: a bus message carries its own
     * length, and the line terminator is stdout's business. */
    dbus_sink_put(line, (len > 0) ? len - 1 : len);
#endif
}


void
put_data(const char *type, const char *fmt, ...)
{
    int errno_saved = errno;
    char fields[512];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(fields, sizeof(fields), fmt, ap);
    va_end(ap);
    if (n < 0) {
	errno = errno_saved;
	return;
    }

    struct timespec ts;
    clock_gettime(PUT_CLOCK, &ts);

    char line[768];
    n = snprintf(line, sizeof(line), "%s %s %lld%09ld\n",
		 type, fields, (long long)ts.tv_sec, ts.tv_nsec);
    if (n > 0) {
	PUT_LOCK()
	put_line(line, ((size_t)n < sizeof(line)) ? (size_t)n
						  : sizeof(line) - 1);
	PUT_UNLOCK()
    }
    errno = errno_saved;
}


void
put_fail(const char *type, const char *failure)
{
    int errno_saved = errno;

    struct timespec ts;
    clock_gettime(PUT_CLOCK, &ts);

    /* Quoted: a field value which is neither a number, a boolean nor a
     * quoted string is a parse error, and the whole line is refused. */
    char line[768];
    int  n = snprintf(line, sizeof(line), "%s failure=\"%s\" %lld%09ld\n",
		      type, failure, (long long)ts.tv_sec, ts.tv_nsec);
    if (n > 0) {
	PUT_LOCK()
	put_line(line, ((size_t)n < sizeof(line)) ? (size_t)n
						  : sizeof(line) - 1);
	PUT_UNLOCK()
    }
    errno = errno_saved;
}

#endif	/* WITH_LINEPROTOCOL || WITH_DBUS */


/************************************************************************
 * Environment                                                          *
 ************************************************************************/

/*
 * An environment variable, this project's spelling first.
 *
 * MQTT_HOST and its friends are generic names, which is both why they
 * were chosen and why they are not enough on their own: a machine
 * carrying more than one MQTT client has one environment between them,
 * and pointing this one at a different broker should not mean moving
 * everything else. MOSES_MQTT_HOST is therefore looked for first.
 *
 * The bare name stays as the fallback, so nothing that was configured
 * before stops working, and the order is that way round because the
 * specific should beat the general -- someone who sets the prefixed name
 * meant it for moses in particular.
 */
static char *
env_moses(const char *name)
{
    char buf[64];
    int  n = snprintf(buf, sizeof(buf), "MOSES_%s", name);
    if ((n < 0) || ((size_t)n >= sizeof(buf)))
	return getenv(name);		/* not a name this tree uses */

    char *v = getenv(buf);
    return (v != NULL) ? v : getenv(name);
}



/************************************************************************
 * Topics                                                               *
 ************************************************************************/

// Where every topic name starts. Not behind WITH_MQTT: the prefix names
// the installation as well as its topics, and moses_display reads the
// last segment of it for the name on the panel.
const char *
mqtt_topic_prefix(void)
{
    const char *prefix = env_moses("MQTT_TOPIC_PREFIX");
    return prefix ? prefix : MQTT_TOPIC_PREFIX;
}



/************************************************************************
 * Mosquitto                                                            *
 ************************************************************************/

/*
 * Everything below is compiled out by WITH_MQTT=OFF, and replaced by the
 * stubs at the foot of this file.
 *
 * Nothing in the daemons changes for that, because they already cope
 * with a broker that was never configured: mqtt_connect() answers 0 for
 * "disabled" and each of them treats it as success, and MQTT_PUBLISH()
 * reaches a mqtt_publish() that returns early with no handler. Compiling
 * it out only makes that state permanent -- and drops libmosquitto from
 * the link, which is the point on a machine that wants nothing but the
 * line protocol on stdout.
 */
#ifdef WITH_MQTT

// Callback called when the client receives a CONNACK message from the broker.
static void
_mqtt_on_connect(struct mosquitto *mosq, void *obj, int reason_code)
{
    struct mqtt *mqtt = obj;
    
    if (reason_code != 0) {
	// mosquitto_connack_string() produces an appropriate
	// string for MQTT v3.x clients, the equivalent for MQTT v5.0
	// clients is mosquitto_reason_string().
	if (mqtt->connection_retry != 0) {
	    if (mqtt->connection_retry > 0)
		mqtt->connection_retry--;
	    LOG("connection failed [RETRYING] (%s)",
		mosquitto_connack_string(reason_code));
	} else {
	    LOG("connection failed [DISCONNECTING] (%s)",
		mosquitto_connack_string(reason_code));
	    mosquitto_disconnect(mosq);
	}
	return;
    }

    // Reset retry counter
    mqtt->connection_retry = mqtt->cfg.connection_max_retry;

    // Announce we are online (retained), so a freshly connecting client
    // immediately knows the program is alive. Mirrors the last will set in
    // mqtt_start(): the broker publishes the offline payload for us if the
    // connection drops unexpectedly.
    if (mqtt->avail.topic) {
	int rc = mosquitto_publish(mosq, NULL, mqtt->avail.topic,
				   strlen(mqtt->avail.online), mqtt->avail.online,
				   mqtt->avail.qos, true);
	if (rc != MOSQ_ERR_SUCCESS)
	    LOG_ERRMQTT_PUBLISH(rc, mqtt->avail.topic);
    }

    // Making subscriptions in the on_connect() callback means that if the
    // connection drops and is automatically resumed by the client,
    // then the subscriptions will be recreated when the client reconnects.
    for (unsigned int i = 0 ; i < mqtt->subcount ; i++) {
	int rc = mosquitto_subscribe(mosq, NULL,
				     mqtt->sub[i].topic, mqtt->sub[i].qos);
	if (rc != MOSQ_ERR_SUCCESS) {
	    // Disconnect if we were unable to subscribe
	    LOG_ERRMQTT(rc, "subscribing failed [DISCONNECTING]");
	    mosquitto_disconnect(mosq);
	    return;
	}
    }
}


int
mqtt_init(struct mqtt *mqtt, unsigned int subcount,
	  struct mqtt_subscription *sub)
{
    // Sanity check
    if (mqtt->cfg.host == NULL) {
	LOG("MQTT not enabled");
	return 0;
    }

    // Deal with subscriptions
    if ((subcount != 0) && (sub != NULL)) {
	mqtt->subcount = subcount;
	mqtt->sub      = calloc(subcount, sizeof(struct mqtt_subscription));
	if (mqtt->sub != NULL) {
	    memcpy(mqtt->sub, sub, subcount * sizeof(struct mqtt_subscription));
	} else {
	    LOG("unable to allocate memory");
	    return -1;
	}
    }
    
    // Mosquitto library initialization
    if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
	LOG("unable to initialize MQTT library");
	return -1;
    }
    
    // Create a new client instance.
    mqtt->mosq = mosquitto_new(mqtt->cfg.client_id, true, mqtt);
    if (mqtt->mosq == NULL) {
	errno = ENOMEM;
	LOG_ERRNO("unable to instance MQTT (Mosquitto) instance");
	mosquitto_lib_cleanup();
	return -1;;
    }

    // Callbacks
    mosquitto_connect_callback_set(mqtt->mosq, _mqtt_on_connect);
    

    // Done
    return 1;
}

int
mqtt_destroy(struct mqtt *mqtt)
{
    if (mqtt->mosq)
	mosquitto_destroy(mqtt->mosq);
    mqtt->mosq = NULL;

    free(mqtt->sub);
    mqtt->sub      = NULL;
    mqtt->subcount = 0;
    return 0;
}


// Configure an availability (online/offline) topic. The strings are borrowed,
// not copied, so they must outlive the MQTT handler. Call before mqtt_start().
void
mqtt_set_availability(struct mqtt *mqtt, char *topic,
		      char *online, char *offline, int qos)
{
    mqtt->avail.topic   = topic;
    mqtt->avail.online  = online;
    mqtt->avail.offline = offline;
    mqtt->avail.qos     = qos;
}


int
mqtt_connect(struct mqtt *mqtt,
	     unsigned int subcount, struct mqtt_subscription *sub,
	     char *avail_topic, mqtt_message_cb on_message)
{
    // Create the client and register subscriptions (0 = MQTT disabled).
    int rc = mqtt_init(mqtt, subcount, sub);
    if (rc <= 0)
	return rc;

    // Advertise liveness: "online" on connect, "offline" as last will, so a
    // crash or lost link is visible to Home Assistant et al.
    if (avail_topic)
	mqtt_set_availability(mqtt, avail_topic, "online", "offline", 1);

    if (on_message)
	mosquitto_message_callback_set(mqtt->mosq, on_message);

    // Connect and start the background network loop.
    if (mqtt_start(mqtt) < 0) {
	mqtt_destroy(mqtt);
	return -1;
    }

    return 1;
}



int
mqtt_publish(struct mqtt *mqtt, const char *topic, int qos, bool retain,
	     const char *fmt, ...)
{
    if ((mqtt == NULL) || (mqtt->mosq == NULL) || (topic == NULL))
	return 0;

    int rc = -1;

    va_list ap;
    va_start(ap, fmt);
    char *data  = NULL;
    int datalen = vasprintf(&data, fmt, ap);
    va_end(ap);
    
    if (datalen < 0) {
	LOG("failed to allocate memory for asprintf");
    } else {
	rc = mosquitto_publish(mqtt->mosq, NULL, topic,
			       datalen, data, qos, retain);
	if (rc != MOSQ_ERR_SUCCESS) {
	    LOG_ERRMQTT_PUBLISH(rc, topic);
	} else {
	    rc = 1;
	}
	
	free(data);
    }

    return rc;
}
	     
bool
mqtt_enabled(const struct mqtt *mqtt)
{
    return mqtt->cfg.host != NULL;
}

/*
 * Clear a variable under both spellings.
 *
 * Both, because either one could be the one that was set, and leaving
 * the other behind would defeat the point of clearing it at all.
 */
static void
env_moses_clear(const char *name)
{
    char buf[64];
    int  n = snprintf(buf, sizeof(buf), "MOSES_%s", name);
    if ((n >= 0) && ((size_t)n < sizeof(buf)))
	unsetenv(buf);
    unsetenv(name);
}


/*
 * A credential: copied out, then cleared from the environment.
 *
 * Copied first because getenv() returns a pointer into the environment
 * and unsetenv() is free to release what it points at -- the shape this
 * replaces handed that pointer straight to mosquitto and then cleared
 * the variable, which held only as long as the libc happened to keep the
 * string alive. With two spellings to clear there is more to go wrong,
 * so the value is taken out of the environment before either is removed.
 *
 * The copy is never freed: it lives as long as the config does, which is
 * as long as the program.
 *
 * Cleared because an environment is inherited by anything the daemon
 * spawns, and on some systems is readable from outside the process.
 */
static char *
env_moses_take(const char *name)
{
    const char *v    = env_moses(name);
    char       *copy = (v != NULL) ? strdup(v) : NULL;

    env_moses_clear(name);
    return copy;
}


void
mqtt_config_from_env(struct mqtt *mqtt)
{
    struct mqtt_config *cfg = &mqtt->cfg;

    // Use environment variable to overide default parameters
    char *s_host      = env_moses("MQTT_HOST");
    char *s_port      = env_moses("MQTT_PORT");
    char *s_client_id = env_moses("MQTT_CLIENT_ID");
    if (s_host) {
	cfg->host = s_host;
    }
    if (s_port) {
	char *endptr;
	long port = strtol(s_port, &endptr, 10);
	if ((*s_port == '\0') || (*endptr != '\0') ||
	    (port <= 0) || (port > 65535))
	    USAGE_DIE("invalid MQTT port number (1..65535)");
	cfg->port = port;
    }
    if (s_client_id) {
	cfg->client_id = s_client_id;
    }
    cfg->username = env_moses_take("MQTT_USERNAME");
    cfg->password = env_moses_take("MQTT_PASSWORD");
}

int
mqtt_start(struct mqtt *mqtt) 
{
    int rc;

    
    // If host not defined, mosquitto is disabled
    if (mqtt->cfg.host == NULL)
	return -1;

    // Set retry
    mqtt->connection_retry = mqtt->cfg.connection_max_retry;

    // Set options
    mosquitto_int_option(mqtt->mosq, MOSQ_OPT_TCP_NODELAY, 1);
    
    // Username / password
    rc = mosquitto_username_pw_set(mqtt->mosq,
				   mqtt->cfg.username, mqtt->cfg.password);
    if (rc != MOSQ_ERR_SUCCESS) {
	LOG_ERRMQTT(rc, "failed to set username/password for MQTT");
	return -1;
    }

    // Last will: the broker publishes the offline payload (retained) on our
    // behalf if the connection drops without a clean disconnect. Must be set
    // before connecting. The matching online payload is published from the
    // on_connect callback.
    if (mqtt->avail.topic) {
	rc = mosquitto_will_set(mqtt->mosq, mqtt->avail.topic,
				strlen(mqtt->avail.offline), mqtt->avail.offline,
				mqtt->avail.qos, true);
	if (rc != MOSQ_ERR_SUCCESS) {
	    LOG_ERRMQTT(rc, "failed to set MQTT last will");
	    return -1;
	}
    }

    // Connect
    rc = mosquitto_connect(mqtt->mosq,
			   mqtt->cfg.host, mqtt->cfg.port, mqtt->cfg.keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
	LOG_ERRMQTT(rc, "unable to connect to MQTT server");
	return -1;
    }

    // Run the network loop in a background thread
    rc = mosquitto_loop_start(mqtt->mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
	LOG_ERRMQTT(rc, "starting MQTT loop failed");
	return -1;
    }
    
    // Done
    return 0;
}

#else	/* ! WITH_MQTT */

/*
 * MQTT compiled out.
 *
 * These are the answers the rest of the tree already knows how to
 * handle, made permanent. mqtt_config_from_env() deliberately does
 * nothing, so cfg.host stays NULL and mqtt_enabled() is false without
 * being told to be: a build without MQTT cannot be talked into thinking
 * it has a broker by an MQTT_HOST left in the environment.
 */

int
mqtt_publish(struct mqtt *mqtt, const char *topic, int qos, bool retain,
	     const char *fmt, ...)
{
    (void)mqtt; (void)topic; (void)qos; (void)retain; (void)fmt;
    return 0;
}

int
mqtt_init(struct mqtt *mqtt, unsigned int subcount,
	  struct mqtt_subscription *sub)
{
    (void)mqtt; (void)subcount; (void)sub;
    return 0;
}

int
mqtt_start(struct mqtt *mqtt)
{
    (void)mqtt;
    return -1;
}

int
mqtt_destroy(struct mqtt *mqtt)
{
    (void)mqtt;
    return 0;
}

void
mqtt_set_availability(struct mqtt *mqtt, char *topic,
		      char *online, char *offline, int qos)
{
    (void)mqtt; (void)topic; (void)online; (void)offline; (void)qos;
}

int
mqtt_connect(struct mqtt *mqtt,
	     unsigned int subcount, struct mqtt_subscription *sub,
	     char *avail_topic, mqtt_message_cb on_message)
{
    (void)mqtt; (void)subcount; (void)sub; (void)avail_topic;
    (void)on_message;
    return 0;				/* 0 is "disabled", not an error */
}

void
mqtt_config_from_env(struct mqtt *mqtt)
{
    (void)mqtt;
}

bool
mqtt_enabled(const struct mqtt *mqtt)
{
    (void)mqtt;
    return false;
}

#endif	/* WITH_MQTT */
