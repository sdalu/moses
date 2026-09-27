#ifndef __COMMON_H
#define __COMMON_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#ifdef WITH_MQTT
#include <mosquitto.h>
#else
// Without the library there is still an API shaped around it, and these
// are only ever pointed at -- struct mqtt keeps a `struct mosquitto *`
// that is always NULL, and the message callback takes pointers it is
// never called with. An incomplete type is enough for both.
struct mosquitto;
struct mosquitto_message;
#endif

struct gpio_v2_line_request;            // <linux/gpio.h>, only consumers need it

/************************************************************************
 * Helpers                                                              *
 ************************************************************************/

#ifndef __arraycount
#define __arraycount(__x)       (sizeof(__x) / sizeof(__x[0]))
#endif

#define MQTT_INITIALIZER()					\
    { .cfg.port                 = 1883,				\
      .cfg.keepalive            = 60,				\
      .cfg.connection_max_retry = -1,				\
    }

#define MQTT_ADJUST_TOPIC(mqtt, _topic, prefix)	do {			\
	char *str;							\
	if (asprintf(&str, "%s/%s", prefix, (mqtt)->topic._topic) < 0)	\
	    DIE(2, "failed to allocate MQTT topic string");		\
	(mqtt)->topic._topic = str;					\
    } while(0)

#define MQTT_ERROR_MSG(source, type, msg)			        \
    "{ "							        \
      "\"source\"" ": \"" source  "\", "			        \
      "\"type\""   ": \"" type    "\", "			        \
      "\"msg\""    ": \"" msg    "\""			        \
    " }"

#define MQTT_TOPIC_ENABLED(mqtt, _topic)				\
    if ((mqtt)->handler.mosq && (mqtt)->topic._topic)

#define MQTT_PUBLISH(mqtt, _topic, qos, retain, fmt, ...)		\
    mqtt_publish(&(mqtt)->handler, (mqtt)->topic._topic, qos, retain,	\
		 fmt __VA_OPT__(,) __VA_ARGS__)



/************************************************************************
 * Log and debug                                                        *
 ************************************************************************/

#ifndef WITH_LOG
// Expand to a do/while so `if (cond) LOG(...);` is a real statement and
// does not trip -Wempty-body when logging is compiled out.
#define LOG(x, ...) do { } while (0)
#endif

#ifndef LOG
#include <stdio.h>
#include <errno.h>
#define LOG(x, ...) do {						\
	int errno_saved = errno;					\
	fprintf(stderr, x "\n", ##__VA_ARGS__);				\
	errno = errno_saved;						\
    } while(0)
#endif

#ifndef LOG_ERRNO
#define LOG_ERRNO(x, ...)						\
	LOG(x " (%s)", ##__VA_ARGS__, strerror(errno))
#endif

#ifndef LOG_ERRMQTT
#ifdef WITH_MQTT
#define LOG_ERRMQTT(err, x, ...)					\
    LOG(x " (%s)", ##__VA_ARGS__, mosquitto_strerror(err))
#else
// mosquitto_strerror() is in the library that is not linked. Nothing
// reaches this without MQTT, but the macro has to compile.
#define LOG_ERRMQTT(err, x, ...)					\
    LOG(x " (MQTT compiled out)", ##__VA_ARGS__)
#endif
#endif

#ifndef LOG_ERRMQTT_PUBLISH
#define LOG_ERRMQTT_PUBLISH(rc, topic)					\
    LOG_ERRMQTT(rc, "failed to publish on topic %s", topic)
#endif

#ifndef ASSERT
#include <assert.h>
#define ASSERT(x)							\
    assert(x)
#endif



/************************************************************************
 * Output                                                               *
 ************************************************************************/

/*
 * Each reading as one line of InfluxDB line protocol:
 *
 *     <measurement> <field>[,<field>...] <nanosecond timestamp>
 *
 *     watermeter index=213044.000 1790430000000000000
 *     environment temperature=21.42,pressure=102134 1790430000000000000
 *     watermeter failure="read" 1790430000000000000
 *
 * The quotes around a failure are not decoration: a field value that is
 * not a number, a boolean or a quoted string is a parse error, and the
 * whole line is refused. Unquoted, `failure=set-state` could not be
 * read as anything at all, so these lines were silently dropped by
 * anything consuming them.
 */

#ifdef WITH_LINEPROTOCOL

#ifndef PUT_LOCK
#define PUT_LOCK()
#endif

#ifndef PUT_UNLOCK
#define PUT_UNLOCK()
#endif

#ifndef PUT_CLOCK
#define PUT_CLOCK CLOCK_TAI
#endif

#ifndef PUT_DATA
#define PUT_DATA(type, fmt, ...) do {					\
	int errno_saved = errno;					\
	PUT_LOCK()							\
	struct timespec ts;						\
	clock_gettime(PUT_CLOCK, &ts);					\
	fprintf(stdout, "%s " fmt " %lld%09ld" "\n",			\
		type __VA_OPT__(,) __VA_ARGS__,				\
		(long long)ts.tv_sec, ts.tv_nsec);			\
	fflush(stdout);							\
	PUT_UNLOCK()							\
	errno = errno_saved;						\
    } while(0)
#endif

#ifndef PUT_FAIL
#define PUT_FAIL(type, failure) do {					\
	int errno_saved = errno;					\
	PUT_LOCK()							\
	struct timespec ts;						\
	clock_gettime(PUT_CLOCK, &ts);					\
	fprintf(stdout, "%s failure=\"%s\" %lld%09ld\n",		\
		type, failure, (long long)ts.tv_sec, ts.tv_nsec);	\
	fflush(stdout);							\
	PUT_UNLOCK()							\
	errno = errno_saved;						\
    } while(0)
#endif

#else

#define PUT_DATA(type, fmt, ...)
#define PUT_FAIL(type, failure)

#endif


/************************************************************************
 * Argument line parsing                                                *
 ************************************************************************/

#define USAGE_DIE(x, ...) do {						\
	fprintf(stderr, x "\n", ##__VA_ARGS__);				\
	exit(1);							\
    } while(0)



/************************************************************************
 * Misc                                                                 *
 ************************************************************************/

// The program name DIE() reports with. Provided by libc, and set from
// basename(argv[0]) in main(); declared here so DIE() can be used from
// any of a program's translation units and not just the one holding
// main. The three daemons also define one of their own, which shadows
// libc's on glibc -- do not copy that into anything new, because on a
// libc that puts __progname in crt1.o (FreeBSD) it is a duplicate
// symbol and will not link.
extern char *__progname;

#define DIE(code, fmt, ...)						\
    do {								\
	fprintf(stderr, "%s: " fmt "\n" , __progname			\
		__VA_OPT__(,) __VA_ARGS__);				\
	exit(code);							\
    } while(0)




/************************************************************************
 * Types                                                                *
 ************************************************************************/

struct mqtt_config {
    char    *host;                      // host
    int      port;                      // port
    char    *client_id;                 // Client ID
    char    *username;                  // username
    char    *password;                  // password
    int      keepalive;                 // keep alive (>= 5)
    int      connection_max_retry;      // max retry (-1 = infinite)
};

struct mqtt_availability {              // Availability (LWT)
    char *topic;                        //  - topic (NULL = disabled)
    char *online;                       //  - payload published on connect
    char *offline;                      //  - payload set as last will
    int   qos;                          //  - QoS for both
};

struct mqtt {                           // MQTT
    struct mosquitto         *mosq;     //  - mosquitto handler
    struct mqtt_config        cfg;      //  - mosquitto config
    unsigned int              subcount; //  - subscription count
    struct mqtt_subscription *sub;      //  - subscription list
    int               connection_retry; //  - current retry
    struct mqtt_availability  avail;    //  - availability (LWT)
};

struct mqtt_subscription {
    char *topic;
    int   qos;
};

/************************************************************************
 * Prototypes                                                           *
 ************************************************************************/

int parse_mbus_baudrate(const char *option, long *val);
int parse_s_period(const char *option, uint64_t *val);
int parse_us_period(const char *option, uint64_t *val);
int parse_idle_timeout(const char *option, unsigned long *val);
int parse_gpio(const char *option, char **chip_id, uint32_t *pin_id);
int parse_gpio_debounce(const char *option, uint32_t *val);
int parse_gpio_edge(const char *option, uint64_t *flags);
int parse_gpio_bias(const char *option, uint64_t *flags);
int parse_gpio_mode(const char *option, uint64_t *flags);
int parse_gpio_active(const char *option, uint64_t *flags);

int __attribute__ ((format(printf, 5, 6)))
mqtt_publish(struct mqtt *mqtt, const char *topic, int qos, bool retain,
	     const char *fmt, ...);

int mqtt_init(struct mqtt *mqtt, unsigned int subcount,
	      struct mqtt_subscription *sub);
int mqtt_start(struct mqtt *mqtt);
int mqtt_destroy(struct mqtt *mqtt);

void mqtt_set_availability(struct mqtt *mqtt, char *topic,
			   char *online, char *offline, int qos);

typedef void (*mqtt_message_cb)(struct mosquitto *mosq, void *obj,
				const struct mosquitto_message *msg);

// init + (optional availability LWT) + (optional message callback) + start,
// destroying the handler on failure. avail_topic / on_message may be NULL.
// Returns 1 when connected, 0 when MQTT is disabled (no host configured),
// or -1 on error.
int mqtt_connect(struct mqtt *mqtt,
		 unsigned int subcount, struct mqtt_subscription *sub,
		 char *avail_topic, mqtt_message_cb on_message);

void mqtt_config_from_env(struct mqtt *mqtt);

// True when MQTT is configured (a host is set). When false the daemon runs
// without publishing, so the topic plumbing and logging can be skipped.
bool mqtt_enabled(const struct mqtt *mqtt);

// Topic prefix from the MQTT_TOPIC_PREFIX environment variable, falling back
// to the compiled-in default. Pass the result to MQTT_ADJUST_TOPIC().
const char *mqtt_topic_prefix(void);

// Open a single GPIO line on chip "/dev/<chip>". The caller fills req->config
// (flags and attrs); this sets the single-line plumbing (num_lines, offset,
// consumer label), issues GPIO_V2_GET_LINE_IOCTL and leaves the line fd in
// req->fd. Returns the controller (chip) fd on success, -1 on failure.
int gpio_open_line(const char *chip, uint32_t pin, const char *label,
		   struct gpio_v2_line_request *req);

// Sleep until the absolute deadline on the given clock, restarting if a signal
// interrupts the wait.
void sleep_until(clockid_t clock, const struct timespec *deadline);

void reduced_latency(void);

#endif
