/*
 * moses_display -- the UPS source
 *
 * upsd, asked on the loopback in NUT's own line protocol, on an
 * interval, from a thread of its own.
 *
 * It is read directly rather than through MQTT because there is nothing
 * on MQTT to read: nut-notify (README, *nut*) forwards four events --
 * ONLINE, ONBATT, LOWBATT, SHUTDOWN -- when upsmon fires them, and
 * publishes no state, no charge and no time. The display wants a figure
 * on the screen at all times, which only upsd has. The events are still
 * worth having, and source-mqtt.c subscribes to them, so that going
 * onto battery repaints at once instead of up to one interval later.
 *
 * Asking is one LIST VAR rather than a GET VAR per variable: it is one
 * round trip, and the reply names each variable, so nothing depends on
 * the order the answers come back in.
 *
 * What a reading means -- whether it is on battery, and how long it has
 * left -- is ups_estimate.c, so that it can be tested without a upsd to
 * ask. This file is the asking.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#include "model.h"
#include "source.h"
#include "ups_estimate.h"


/* upsd's port, and how long it is given to answer. It is on the
 * loopback: if it has not replied in two seconds it is not going to,
 * and this thread has an interval to keep. */
#define NUT_PORT	3493
#define NUT_TIMEOUT	2

/* One protocol line. Longest realistic is a ups.status with every flag
 * set, well inside this. */
#define NUT_LINE	512

/* Seconds in an hour, spelled out where the estimate divides by it. */
#define PER_HOUR	3600.0


static char          nut_ups[64];
static unsigned long nut_interval = 10;
static pthread_t     nut_thread;


const char *
source_nut_name(void)
{
    return (nut_ups[0] != '\0') ? nut_ups : NULL;
}


//== The line protocol =================================================

/* Read one line, with its terminator taken off. */
static bool
nut_line(FILE *in, char *line, size_t len)
{
    if (fgets(line, (int)len, in) == NULL)
	return false;

    line[strcspn(line, "\r\n")] = '\0';
    return true;
}


/*
 * Open a session with upsd.
 *
 * Hands back a stdio stream for reading; the same descriptor is written
 * to directly, which is fine because nothing is ever buffered for
 * writing on it.
 */
static FILE *
nut_open(int *fd_out)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
	return NULL;

    struct timeval wait = { .tv_sec = NUT_TIMEOUT };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait));

    struct sockaddr_in sa = {
	.sin_family      = AF_INET,
	.sin_port        = htons(NUT_PORT),
	.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    FILE *in;
    if ((connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) ||
	((in = fdopen(fd, "r")) == NULL)) {
	close(fd);
	return NULL;
    }

    *fd_out = fd;
    return in;
}


static void
nut_close(FILE *in, int fd)
{
    /* Polite, not required: upsd drops the session with the socket. */
    ssize_t bye = write(fd, "LOGOUT\n", 7);
    (void)bye;
    fclose(in);				/* closes fd */
}


static bool
nut_send(int fd, const char *cmd)
{
    size_t len = strlen(cmd);

    return write(fd, cmd, len) == (ssize_t)len;
}


/* The first UPS upsd knows, into nut_ups. */
static bool
nut_discover(FILE *in, int fd)
{
    char line[NUT_LINE];

    if (! nut_send(fd, "LIST UPS\n"))
	return false;

    while (nut_line(in, line, sizeof(line))) {
	if ((strncmp(line, "END LIST", 8) == 0) ||
	    (strncmp(line, "ERR", 3) == 0))
	    break;
	if (nut_ups[0] == '\0')
	    sscanf(line, "UPS %63s", nut_ups);
    }
    return nut_ups[0] != '\0';
}


//== One reading =======================================================

/*
 * One LIST VAR, parsed into `out`.
 *
 * The reply is a BEGIN LIST VAR line, then one `VAR <ups> <name> "<value>"`
 * per variable, then END LIST VAR. Anything unrecognised is skipped, so
 * a driver reporting more than this needs changes nothing.
 */
static bool
nut_read_vars(FILE *in, int fd, struct ups_reading *out, char *status,
	      size_t status_len)
{
    char line[NUT_LINE], ask[128];

    status[0] = '\0';
    *out = (struct ups_reading){
	.status = status,
	.charge = -1, .runtime = -1, .capacity = -1, .current = NAN,
    };

    int n = snprintf(ask, sizeof(ask), "LIST VAR %s\n", nut_ups);
    if ((n < 0) || ((size_t)n >= sizeof(ask)) || (! nut_send(fd, ask)))
	return false;

    bool ok = false;
    while (nut_line(in, line, sizeof(line))) {
	if (strncmp(line, "END LIST", 8) == 0) {
	    ok = true;
	    break;
	}
	if (strncmp(line, "ERR", 3) == 0)
	    break;

	char name[64], value[128];
	if (sscanf(line, "VAR %*s %63s \"%127[^\"]\"", name, value) != 2)
	    continue;

	if      (strcmp(name, "ups.status")       == 0) {
	    /* Truncating on purpose: a status longer than the model's
	     * field would not fit across the panel either. Spelled out
	     * as a bounded copy rather than left to snprintf, which
	     * cannot tell a truncation that was meant from one that was
	     * not, and warns about both (-Wformat-truncation). */
	    size_t len = strlen(value);
	    if (len >= status_len)
		len = status_len - 1;
	    memcpy(status, value, len);
	    status[len] = '\0';
	}
	else if (strcmp(name, "battery.charge")   == 0)
	    out->charge   = strtod(value, NULL);
	else if (strcmp(name, "battery.runtime")  == 0)
	    out->runtime  = strtod(value, NULL);
	else if (strcmp(name, "battery.capacity") == 0)
	    out->capacity = strtod(value, NULL);
	else if (strcmp(name, "battery.current")  == 0)
	    out->current  = strtod(value, NULL);
    }
    return ok;
}


/*
 * Ask once and write the model.
 *
 * Three outcomes, kept apart because they mean different things: no
 * upsd at all is a machine without a UPS and not a fault; an upsd that
 * answers but will not say what its UPS is doing is a fault, because
 * something is being watched and would not see a power cut coming; and
 * a reading is a reading.
 */
static void
nut_poll(void)
{
    int   fd = -1;
    FILE *in = nut_open(&fd);

    if (in == NULL) {
	model_set_ups_absent();
	return;
    }

    if ((nut_ups[0] == '\0') && (! nut_discover(in, fd))) {
	nut_close(in, fd);
	model_set_ups_absent();
	return;
    }

    struct ups_reading v;
    char               status[MODEL_STATUS_MAX];
    if ((! nut_read_vars(in, fd, &v, status, sizeof(status))) ||
	(status[0] == '\0')) {
	nut_close(in, fd);
	model_set_ups_lost();
	return;
    }
    nut_close(in, fd);

    bool   estimated;
    double runtime = ups_runtime(&v, &estimated);

    model_set_ups(status, v.charge, runtime, estimated);
}


//== Source ============================================================

void
source_nut_probe(const char *ups)
{
    if (ups != NULL)
	snprintf(nut_ups, sizeof(nut_ups), "%s", ups);

    nut_poll();

    if (nut_ups[0] != '\0')
	LOG("UPS                  : %s (upsd on localhost:%d)",
	    nut_ups, NUT_PORT);
    else
	LOG("UPS                  : none (no upsd on localhost:%d)",
	    NUT_PORT);
}


static void *
nut_loop(void *arg)
{
    (void)arg;

    struct timespec next;
    clock_gettime(CLOCK_REALTIME, &next);

    for (;;) {
	next.tv_sec += (time_t)nut_interval;
	sleep_until(CLOCK_REALTIME, &next);
	nut_poll();
    }
    return NULL;
}


int
source_nut_start(const char *ups, unsigned long interval)
{
    if (ups != NULL)
	snprintf(nut_ups, sizeof(nut_ups), "%s", ups);
    if (interval > 0)
	nut_interval = interval;

    int rc = pthread_create(&nut_thread, NULL, nut_loop, NULL);
    if (rc != 0) {
	errno = rc;
	LOG_ERRNO("failed to start the UPS polling thread");
	return -1;
    }

    /* Nothing ever joins it: it runs for the life of the program, and
     * the program ends by exiting. */
    pthread_detach(nut_thread);
    return 0;
}
