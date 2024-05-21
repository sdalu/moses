#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <sys/mman.h>

#include <linux/gpio.h>
#include <mosquitto.h>

#include "common.h"

/************************************************************************
 * Raspberry PI GPIO defintions                                         *
 ************************************************************************/

/* RPI gpio chipset */
#define RPI_GPIO_CHIP		"gpiochip0"

/* RPI pin mapping (-1 are power or ground pin) */
static int rpi_pinmap[] = {
    -1, -1,  2, -1,  3, -1,  4, 14, -1, 15,
    17, 18, 27, -1, 22, 23, -1, 24, 10, -1,
     9, 25, 11,  8, -1,  7,  0,  1,  5, -1,
     6, 12, 13, -1, 19, 16, 26, 20, -1, 21 };


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

    char    *end = NULL;
    uint64_t   v = strtoull(option, &end, 10);
    if ((v == ULLONG_MAX) && (errno != 0)) return -1;

    if      (*end == '\0'           ) { v *=      1ull; }
    else if (strcmp(end, "s"  ) == 0) { v *=      1ull; }
    else if (strcmp(end, "min") == 0) { v *=     60ull; }
    else if (strcmp(end, "h"  ) == 0) { v *=   3600ull; }
    else if (strcmp(end, "d"  ) == 0) { v *=  86400ull; }
    else if (strcmp(end, "w"  ) == 0) { v *= 604800ull; }
    else                              { return -1;      }
    
    *val = v;
    return 0;
}


int
parse_us_period(const char *option, uint64_t *val)
{
    _Static_assert(sizeof(long long) ==	sizeof(uint64_t));

    char    *end = NULL;
    uint64_t   v = strtoull(option, &end, 10);
    if ((v == ULLONG_MAX) && (errno != 0)) return -1;

    if      (*end == '\0'           ) { v *=          1ull; }
    else if (strcmp(end, "us" ) == 0) { v *=          1ull; } 
    else if (strcmp(end, "ms" ) == 0) { v *=       1000ull; }
    else if (strcmp(end, "s"  ) == 0) { v *=    1000000ull; }
    else if (strcmp(end, "min") == 0) { v *=   60000000ull; }
    else if (strcmp(end, "h"  ) == 0) { v *= 3600000000ull; }
    else                              { return -1;          }
    
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
	_chip_id = strdup(RPI_GPIO_CHIP);
	if (_chip_id == NULL) return -1;
	if ((_pin_id < 1) || (_pin_id > 40)) return -1;
	if (rpi_pinmap[_pin_id - 1] < 0) return -1;
	_pin_id = rpi_pinmap[_pin_id - 1];
    }

    if (chip_id) *chip_id = _chip_id;
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
reduced_lattency(void)
{
    LOG("configuring for reduced lattency");
    
    // Change scheduler priority to be more "real-time"
    struct sched_param sp = {
        .sched_priority = sched_get_priority_max(SCHED_FIFO),
    };
    sched_setscheduler(0, SCHED_FIFO, &sp);

    // Avoid swapping by locking page in memory
    mlockall(MCL_CURRENT | MCL_FUTURE);
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
