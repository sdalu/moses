#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <linux/gpio.h>
#include <time.h>
#include <sys/time.h>

#include <getopt.h>
#include <libgen.h>

#include <mosquitto.h>

#include "common.h"

#ifndef timespeccmp
#define timespeccmp(tvp, uvp, cmp)                                      \
        (((tvp)->tv_sec == (uvp)->tv_sec) ?                             \
            ((tvp)->tv_nsec cmp (uvp)->tv_nsec) :                       \
            ((tvp)->tv_sec cmp (uvp)->tv_sec))
#endif


#define NICKNAME "breaker"


//== Structures ========================================================

struct breaker_control {
    struct {                            // Controller
	char       *id;                 //  - identifier
	int         fd;                 //  - file descriptor
    } ctrl;
    struct {                            // Pin
	uint32_t    id;                 //  - identifier
	int         fd;                 //  - file descriptor
	uint64_t    flags;              //  - flags
	char       *label;              //  - label
	int         defval;             //  - default value
    } pin;
    unsigned long   idle_timeout;       // idle timeout in s
    int             state;              // actual state
    pthread_mutex_t mutex;              // mutex
    struct timespec set_time_published; // last pubished set state
};

struct breaker_mqtt {                   // MQTT
    struct mqtt handler;
    struct {
	char *setter;
	char *publish;
	char *error;
    } topic;
};

struct breaker {
    struct breaker_mqtt    mqtt;
    struct breaker_control control;
    int reduced_latency;
};



//== Global context ====================================================

struct breaker breaker =  {
    .mqtt = {
	.handler        = MQTT_INITIALIZER(),
	.topic.setter   = "waterbreaker/state/set",
	.topic.publish  = "waterbreaker/state",
	.topic.error    = "waterbreaker/error",
    },
    .control = {
	.ctrl.id              = NULL,
	.ctrl.fd              = -1,
	.pin.id               = ~0,
	.pin.fd               = -1,
	.pin.flags            = 0,
	.pin.label            = "breaker-control",
    },
};

#define BREAKER_STATE(n, v) { n, sizeof(n) - 1, v }
static struct breaker_state {
    char *name;
    int   namelen;
    int   value;
} breaker_state[] = { BREAKER_STATE("0",     0), BREAKER_STATE("1",     1),
		      BREAKER_STATE("false", 0), BREAKER_STATE("true",  1), 
		      BREAKER_STATE("off",   0), BREAKER_STATE("on",    1),
};


//== Mosquitto callbacks ===============================================
/* QoS : 0 = no guaranty
 *       1 = at least once
 *       2 = exactly once
 */
void on_connect(struct mosquitto *mosq, void *obj, int reason_code);
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);



//======================================================================


int
breaker_control_init(struct breaker_control *bc)
{
    char *devpath = NULL;
    int   rc      = -EINVAL;
    int   fd      = -1;
    
    if (pthread_mutex_init(&bc->mutex, NULL) != 0) {
	LOG_ERRNO("failed to create mutex");
	goto failed_mutex;
    }
    
    // Build device path
    rc = asprintf(&devpath, "/dev/%s", bc->ctrl.id);
    if (rc < 0) {
	errno = ENOMEM;
	LOG_ERRNO("unable to build path to device name");
	goto failed_gpio;
    }

    // Open device
    fd = open(devpath, O_RDONLY);
    if (fd < 0) {
	LOG_ERRNO("failed to open %s", devpath);
	free(devpath);
	goto failed_gpio;
    }
    LOG("controller device %s opened (fd=%d)", devpath, fd);


    // State
    bc->state = bc->pin.defval ? 1 : 0;
    
    // Get line (with a single gpio)
    // 
    struct gpio_v2_line_request req = {
	.num_lines        = 1,
	.offsets          = { [0] = bc->pin.id },
	.config.flags     = GPIO_V2_LINE_FLAG_OUTPUT |  bc->pin.flags,
	.config.num_attrs = 1,
	.config.attrs     = {
	    { .mask        = 1 << 0,
	      .attr.id     = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES,
	      .attr.values = bc->state << 0
	    },
	}
    };
    strncpy(req.consumer, bc->pin.label, sizeof(req.consumer));
    
    // Release memory
    free(devpath);
    
    // Save controller file descriptor
    bc->ctrl.fd = fd;
    
    // Call ioctl
    rc = ioctl(bc->ctrl.fd, GPIO_V2_GET_LINE_IOCTL, &req);
    if (rc < 0) {
	LOG_ERRNO("failed to issue GPIO_V2_GET_LINE IOCTL for pin %d",
		  bc->pin.id);
	goto failed_gpio;
    }
    
    // Store file descriptor
    bc->pin.fd = req.fd;
    LOG("GPIO line configured as single output pin %d (fd=%d)",
	bc->pin.id, bc->pin.fd);
    
    return 0;

 failed_gpio:
    if (bc->ctrl.fd >= 0) close(bc->ctrl.fd);
    if (bc->pin.fd  >= 0) close(bc->pin.fd );
    bc->ctrl.fd = -1;
    bc->pin.fd  = -1;
 failed_mutex:
    pthread_mutex_destroy(&bc->mutex);
    return -1;   
}


//== MQTT ==============================================================

int
breaker_mqtt_init(struct breaker_mqtt *mqtt)
{
    int rc;

    // Initialise (0 = not enabled)
    rc = mqtt_init(&mqtt->handler, 1, &(struct mqtt_subscription) {
	    .topic = mqtt->topic.setter,
	    .qos   = 1,
	});
    if (rc <= 0) return rc;
    
    mosquitto_message_callback_set(mqtt->handler.mosq, on_message);

    // Start
    rc = mqtt_start(&mqtt->handler);
    if (rc < 0) goto failed;

    // Done
    LOG("MQTT connection established");
    return 0;
    
 failed:
    mqtt_destroy(&mqtt->handler);
    return -1;
}

int breaker_init(struct breaker *b) {
    if ((breaker_control_init(&b->control) < 0) ||
	(breaker_mqtt_init(&b->mqtt)       < 0))
	return -1;
    return 0;
}


void
breaker_control_destroy(struct breaker_control *bc) {
    if (bc->ctrl.fd >= 0) close(bc->ctrl.fd);
    if (bc->pin.fd  >= 0) close(bc->pin.fd );
}

void
breaker_mqtt_destroy(struct breaker_mqtt *mqtt) {
    mqtt_destroy(&mqtt->handler);
}

void
breaker_destroy(struct breaker *b) {
    breaker_mqtt_destroy(&b->mqtt);
    breaker_control_destroy(&b->control);
}


int
breaker_set_state(struct breaker *b, int state, bool publish) {
    struct breaker_control *bc   = &b->control;
    struct breaker_mqtt    *mqtt = &b->mqtt;

    // Sanity check
    if (state < 0) {
	errno = EINVAL;
	return -1;
    }

    // Normalize
    state = (state == 0) ? 0 : 1;
	
    // Set line
    struct gpio_v2_line_values values = {
	.mask = 1     << 0,
	.bits = state << 0,
    };
    int rc = ioctl(b->control.pin.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
    if (rc < 0) {
	return -1;
    } 

    // Save state
    pthread_mutex_lock(&bc->mutex);
    b->control.state = state;
    pthread_mutex_unlock(&bc->mutex);
    
    // Publish new state
    if (publish) {
	pthread_mutex_lock(&bc->mutex);
	clock_gettime(INTERVAL_CLOCK, &bc->set_time_published);
	pthread_mutex_unlock(&bc->mutex);
	
	PUT_DATA(NICKNAME, "state=%d", state);
	MQTT_PUBLISH(mqtt, publish, 1, false, "%d", state);
    }

    // Done
    return 0;
    
}


int
breaker_get_state(struct breaker *b) {
    struct breaker_control *bc = &b->control;

    int state;

    pthread_mutex_lock(&bc->mutex);
    state = bc->state;
    pthread_mutex_unlock(&bc->mutex);

    return state;
}



//======================================================================

char *__progname = "??";


static void
breaker_parse_config(int argc, char **argv, struct breaker *b)
{
    struct breaker_control *bc = &b->control;

    static const char *const shortopts = "+rP:L:M:A:I:h";
    
    const struct option longopts[] = {
	{ "reduced-latency", no_argument,	NULL, 'r' },
	{ "pin",             required_argument, NULL, 'P' },
	{ "pin-label",       required_argument, NULL, 'L' },
	{ "mode",            required_argument, NULL, 'M' },
	{ "active",          required_argument, NULL, 'A' },
	{ "idle-timeout",    required_argument, NULL, 'I' },
	{ "help",	     no_argument,	NULL, 'h' },
	{ NULL },
    };

    int opti, optc;
    
    for (;;) {
	optc = getopt_long(argc, argv, shortopts, longopts, &opti);
	if (optc < 0)
	    break;
	
	switch (optc) {
	case 'r':
	    b->reduced_latency = 1;
	    break;
	case 'P':
	    if (parse_gpio(optarg, &bc->ctrl.id, &bc->pin.id) < 0)
		USAGE_DIE("invalid GPIO pin (chipset:pin)");
	    break;
	case 'L':
	    bc->pin.label = optarg;
	    break;
	case 'M':
	    if (parse_gpio_mode(optarg, &bc->pin.flags) < 0)
		USAGE_DIE("invalid mode (as-is, open-drain, open-source, push-pull)");
	    break;
	case 'A':
	    if (parse_gpio_active(optarg, &bc->pin.flags) < 0)
		USAGE_DIE("invalid active state (low, high)");
	    break;
	case 'I':
	    if (parse_idle_timeout(optarg, &bc->idle_timeout) < 0)
		USAGE_DIE("invalid idle timeout (1s .. 10w)");
	    break;
	case 'h':
	    printf("%s [opts]\n", __progname);
	    printf("  -r, --reduced-latency            try to reduce latency\n");
	    printf("  -P, --pin=CTRL:PIN               gpio pulse counting pin\n");
	    printf("  -L, --pin-label=STRING           gpio pin label\n");
	    printf("  -M, --mode=as-is|push-pull|      gpio mode\n");
	    printf("             open-drain|open-source\n");
	    printf("  -A, --active=low|high            gpio active state\n");
	    printf("  -I, --idle-timeout=SEC           notify state if no command send\n");
	    printf("\n");
	    exit(1);
	default:
	    exit(1);
	}
    }

    argc -= optind;
    argv += optind;

    if (b->control.ctrl.id == NULL) {
	USAGE_DIE("A GPIO pin must be selected");
    }
}





int
main(int argc, char **argv)
{
    __progname = basename(argv[0]);

    //Shortcuts
    struct breaker_control *bc = &breaker.control;
    struct breaker_mqtt  *mqtt = &breaker.mqtt;

    // Configuration
    mqtt_config_from_env(&breaker.mqtt.handler);
    breaker_parse_config(argc, argv, &breaker);
    
    // Initialization
    if (breaker_init(&breaker) < 0)
	DIE(2, "failed to initialize");
    
    // Reducing latency
    if (breaker.reduced_latency)
	reduced_lattency();


    /* At this point the client is connected to the network socket, but may not
     * have completed CONNECT/CONNACK.
     * It is fairly safe to start queuing messages at this point, but if you
     * want to be really sure you should wait until after a successful call to
     * the connect callback.
     * In this case we know it is 1 second before we start publishing.
     */

    // Polling
    struct timespec next_polling;
    clock_gettime(INTERVAL_CLOCK, &next_polling);
    while (1) {
	// Check if a set changed was already published
	pthread_mutex_lock(&bc->mutex);
	if (timespeccmp(&next_polling, &bc->set_time_published, <)) {
	    next_polling = bc->set_time_published;
	    pthread_mutex_unlock(&bc->mutex);
	    goto sleep;
	}
	pthread_mutex_unlock(&bc->mutex);

	// Current state
	int state = breaker_get_state(&breaker);
	
	// Publish
	PUT_DATA(NICKNAME, "state=%d", state);
	MQTT_PUBLISH(mqtt, publish, 1, false, "%d", state);

    sleep:
	// Next	polling
	next_polling.tv_sec += breaker.control.idle_timeout;
    sleep_again:
	if (clock_nanosleep(INTERVAL_CLOCK, TIMER_ABSTIME,
			    &next_polling, NULL) < 0) {
	    assert(errno == EINTR);
	    goto sleep_again;
	}
    }

    exit(0);
}



int breaker_parse_state(char *data, int datalen) {
    // Look for NUL-char 
    if (strnlen(data, datalen) != datalen)
	return -1;

    // State lookup
    for (unsigned int i = 0 ; i < __arraycount(breaker_state) ; i++)
	if ((breaker_state[i].namelen == datalen) && 
	    (! strncasecmp(breaker_state[i].name, data, datalen)))
	    return breaker_state[i].value;
    return -1;
}





// Callback called when the client receives a message.
void
on_message(struct mosquitto *mosq, void *obj,
	   const struct mosquitto_message *msg)
{
    struct breaker_mqtt *mqtt = &breaker.mqtt;
    assert(mqtt->handler.mosq == mosq);

    // Setter topic
    if (!strcmp(mqtt->topic.setter, msg->topic)) {
	// Parse requested state
	int state = breaker_parse_state(msg->payload, msg->payloadlen);
	if (state < 0) {
	    LOG("garbage content for MQTT topic %s", msg->topic);
	    return;
	}

	// Set breaker state
	int rc = breaker_set_state(&breaker, state, true);
	if (rc < 0) {
	    LOG("failed to set breaker state!");
	    PUT_FAIL(NICKNAME, "set-state");
	    static char *msg = MQTT_ERROR_MSG(NICKNAME, "critical",
					      "failed to set breaker state");
	    MQTT_PUBLISH(mqtt, error, 2, false, msg);

	}
    }
}

