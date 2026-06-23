/*
 * common.c -- code shared by the moses programs.
 *
 *   - Command-line option parsers (parse_*): baud rate, time periods,
 *     GPIO pin specifications (`chip:pin`, with an `rpi:<header-pin>`
 *     convenience mapping) and the various GPIO line flags.
 *   - reduced_latency(): switch to the SCHED_FIFO real-time scheduler
 *     and lock memory, to keep pulse counting / valve control responsive.
 *   - A thin MQTT wrapper around libmosquitto (mqtt_*): connection,
 *     automatic reconnection with re-subscription, printf-style publish,
 *     and configuration from the MQTT_* environment variables.
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
#include <sys/ioctl.h>
#include <fcntl.h>
#include <dirent.h>

#include <linux/gpio.h>
#include <mosquitto.h>

#include "common.h"

/************************************************************************
 * Raspberry PI GPIO defintions                                         *
 ************************************************************************/

/* RPI pin mapping (-1 are power or ground pin) */
static int rpi_pinmap[] = {
    -1, -1,  2, -1,  3, -1,  4, 14, -1, 15,
    17, 18, 27, -1, 22, 23, -1, 24, 10, -1,
     9, 25, 11,  8, -1,  7,  0,  1,  5, -1,
     6, 12, 13, -1, 19, 16, 26, 20, -1, 21 };

/* Labels of the Raspberry Pi 40-pin header GPIO bank, by SoC. The device
 * node number (gpiochipN) is not stable across models -- notably the Pi 5
 * moved the header to the RP1 -- so the chip is resolved by label instead.
 * The header pins are bank 0 (offsets matching rpi_pinmap) on all of them. */
static const char *const rpi_chip_labels[] = {
    "pinctrl-rp1",      // Pi 5      (RP1)
    "pinctrl-bcm2711",  // Pi 4
    "pinctrl-bcm2835",  // Pi 1-3, Zero
};

/* Return the /dev name (e.g. "gpiochip0") of the header GPIO bank, or NULL
 * if not running on a recognized Raspberry Pi. Caller frees. */
static char *
rpi_gpio_chip(void)
{
    DIR *d = opendir("/dev");
    if (d == NULL) return NULL;

    char *found = NULL;
    struct dirent *e;
    while ((found == NULL) && ((e = readdir(d)) != NULL)) {
	if (strncmp(e->d_name, "gpiochip", 8) != 0) continue;

	char path[sizeof("/dev/") + sizeof(e->d_name)];
	snprintf(path, sizeof(path), "/dev/%s", e->d_name);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) continue;

	struct gpiochip_info info;
	if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0) {
	    for (size_t i = 0; i < __arraycount(rpi_chip_labels); i++)
		if (strcmp(info.label, rpi_chip_labels[i]) == 0) {
		    found = strdup(e->d_name);
		    break;
		}
	}
	close(fd);
    }
    closedir(d);
    return found;
}


int
gpio_open_line(const char *chip, uint32_t pin, const char *label,
	       struct gpio_v2_line_request *req)
{
    // Build device path "/dev/<chip>"
    char *devpath = NULL;
    if (asprintf(&devpath, "/dev/%s", chip) < 0) {
	errno = ENOMEM;
	LOG_ERRNO("unable to build path to device name");
	return -1;
    }

    // Open the GPIO character device
    int fd = open(devpath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
	LOG_ERRNO("failed to open %s", devpath);
	free(devpath);
	return -1;
    }
    LOG("controller device %s opened (fd=%d)", devpath, fd);
    free(devpath);

    // The caller has filled req->config (flags/attrs); we own the
    // single-line plumbing.
    req->num_lines  = 1;
    req->offsets[0] = pin;
    strncpy(req->consumer, label, sizeof(req->consumer) - 1);

    if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, req) < 0) {
	LOG_ERRNO("failed to issue GPIO_V2_GET_LINE IOCTL for pin %u", pin);
	close(fd);
	return -1;
    }
    LOG("GPIO line configured as single pin %u (fd=%d)", pin, req->fd);

    return fd;
}


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



int
parse_gpio(const char *option, char **chip_id, uint32_t *pin_id)
{
    char *sep = strchr(option, ':');
    if (sep == NULL) return -1;
    char *_chip_id = strndup(option, sep - option);
    if (_chip_id == NULL) return -1;
    
    char   * endptr   = NULL;
    char   * number   = sep + 1;
    uint32_t _pin_id  = strtoul(number, &endptr, 10);

    if ((number[0] == '\0') || (endptr[0] != '\0'))
	goto failed;

    if (strcmp(_chip_id, "rpi") == 0) {
	free(_chip_id);
	_chip_id = rpi_gpio_chip();
	if (_chip_id == NULL) return -1;        // not a recognized Pi
	if ((_pin_id < 1) || (_pin_id > 40)) goto failed;
	if (rpi_pinmap[_pin_id - 1] < 0)     goto failed;
	_pin_id = rpi_pinmap[_pin_id - 1];
    }

    if (chip_id) *chip_id = _chip_id;
    else         free(_chip_id);
    if (pin_id ) *pin_id  = _pin_id;

    return 0;
    
 failed:
    free(_chip_id);
    return -1;
}

// Return delay in micro-seconds (range: 1 micro-sec to 1 hour)
int
parse_gpio_debounce(const char *option, uint32_t *val)
{
    uint64_t v;
    if ((parse_us_period(option, &v) <  0            ) ||
	(v                           <= 0            ) ||
	(v                           >  UINT32_MAX   ) ||
	(v                           >  3600000000ull))
	return -1;

    *val = v;
    return 0;
}

int
parse_gpio_edge(const char *option, uint64_t *flags)
{
    static const uint64_t all_flags =
	GPIO_V2_LINE_FLAG_EDGE_RISING |
	GPIO_V2_LINE_FLAG_EDGE_FALLING;

    if        (strcmp(option, "rising" ) == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_EDGE_RISING;
    } else if (strcmp(option, "falling") == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_EDGE_FALLING;
    } else {
	return -1;
    }
    return 0;
}

int
parse_gpio_bias(const char *option, uint64_t *flags)
{
    static const uint64_t all_flags =
	GPIO_V2_LINE_FLAG_BIAS_DISABLED |
	GPIO_V2_LINE_FLAG_BIAS_PULL_UP  |
	GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    
    if        (strcmp(option, "as-is"    ) == 0) {
	*flags &= ~all_flags;
    } else if (strcmp(option, "disabled" ) == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    } else if (strcmp(option, "pull-up"  ) == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    } else if (strcmp(option, "pull-down") == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    } else {
	return -1;
    }
    return 0;
}


int
parse_gpio_mode(const char *option, uint64_t *flags)
{
    static const uint64_t all_flags =
	GPIO_V2_LINE_FLAG_OPEN_DRAIN |
	GPIO_V2_LINE_FLAG_OPEN_SOURCE;
    
    if        (strcmp(option, "as-is"      ) == 0) {
	*flags &= ~all_flags;
    } else if (strcmp(option, "open-drain" ) == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_OPEN_DRAIN;
    } else if (strcmp(option, "open-source") == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_OPEN_SOURCE;
    } else if (strcmp(option, "push-pull" ) == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_OPEN_DRAIN |
	          GPIO_V2_LINE_FLAG_OPEN_SOURCE;
    } else {
	return -1;
    }
    return 0;
}

int
parse_gpio_active(const char *option, uint64_t *flags)
{
    static const uint64_t all_flags = GPIO_V2_LINE_FLAG_ACTIVE_LOW;
    
    if        (strcmp(option, "low" ) == 0) {
	*flags &= ~all_flags;
	*flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
    } else if (strcmp(option, "high") == 0) {
	*flags &= ~all_flags;
    } else {
	return -1;
    }
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
 * Mosquitto                                                            *
 ************************************************************************/

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
mqtt_init(struct mqtt *mqtt, int subcount, struct mqtt_subscription *sub)
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
	     
const char *
mqtt_topic_prefix(void)
{
    const char *prefix = getenv("MQTT_TOPIC_PREFIX");
    return prefix ? prefix : MQTT_TOPIC_PREFIX;
}

void
mqtt_config_from_env(struct mqtt *mqtt)
{
    struct mqtt_config *cfg = &mqtt->cfg;
    
    // Use environment variable to overide default parameters
    char *s_host      = getenv("MQTT_HOST");
    char *s_port      = getenv("MQTT_PORT");
    char *s_client_id = getenv("MQTT_CLIENT_ID");
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
    cfg->username = getenv("MQTT_USERNAME");
    cfg->password = getenv("MQTT_PASSWORD");
    unsetenv("MQTT_USERNAME");
    unsetenv("MQTT_PASSWORD");
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
