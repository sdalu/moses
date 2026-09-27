/*
 * moses_display -- the local datagram source
 *
 * The other half of WITH_DGRAM. The daemons on this machine send every
 * reading to a unix datagram socket as one line of line protocol
 * (src/common.c, dgram_send()); this binds that socket and turns what
 * arrives into the model.
 *
 * Why it is worth having, when MQTT already carries all of this: the
 * broker is not on this machine. A reading taken here otherwise travels
 * over the network to another host and back again to reach a panel ten
 * centimetres away, and when that network is down the panel goes blank
 * while every daemon behind it is working perfectly. That is precisely
 * the moment somebody walks up to it. This path has neither hop and no
 * dependency off the box.
 *
 * It does not replace MQTT, and is not meant to. Two things it cannot
 * carry:
 *
 *   Retention. MQTT's retained `index`, `state` and `availability` are
 *   why the panel fills in within a moment of starting. A datagram
 *   socket has no memory: started between reports, this source shows
 *   nothing until the next one, which can be a minute.
 *
 *   Liveness. There is no last will, so a daemon that dies says nothing.
 *   The availability marks are deliberately left alone here rather than
 *   set from an arriving datagram: a datagram proves a daemon was alive
 *   a moment ago, but nothing would ever clear the mark again, and a
 *   panel claiming "online" about a process that died an hour ago is
 *   worse than one that does not claim to know. Staleness is what covers
 *   it -- every figure carries when it arrived, and the dashboard greys
 *   one that has stopped being refreshed.
 *
 * Parsing lives in lineproto.c, with a test, because it decides what
 * figure reaches the panel. This file is the socket.
 */

/* This file only means anything when the daemons are sending: WITH_DGRAM
 * is what compiles dgram_path() and dgram_send() into them, and what
 * CMakeLists.txt adds this source under. Said here so that getting that
 * wrong is a line to read rather than an undefined reference to chase. */
#ifndef WITH_DGRAM
#error "source-unix.c is only built under WITH_DGRAM"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common.h"

#include "lineproto.h"
#include "model.h"
#include "source.h"


/* Room for the longest line put_data() can build. */
#define DGRAM_MAX	LINEPROTO_MAX

/* The socket is created rw for owner and group: a producer running as
 * another user in a shared group can then write to it, while the world
 * cannot. Nothing on this socket is a secret, but nothing off this
 * machine has any business writing the panel either. */
#define DGRAM_MODE	0660


static int       unix_fd = -1;
static pthread_t unix_thread;


/*
 * Whether something is already bound to `path`.
 *
 * A stale socket file has to be removed before bind() will take the
 * address, and a live one must not be: two displays binding the same
 * path in turn would leave the first receiving nothing, silently, with
 * both of them looking like they work. connect() tells them apart --
 * on an AF_UNIX datagram socket it succeeds against a bound peer and
 * fails with ECONNREFUSED against a file nobody is listening on.
 */
static bool
someone_is_bound(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
	return false;			/* cannot tell; assume not */

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    bool bound = (connect(fd, (const struct sockaddr *)&addr,
			  sizeof(addr)) == 0);
    close(fd);
    return bound;
}


/*
 * Create and bind the socket.
 *
 * @return < 0 with errno set, having logged what went wrong
 */
static int
unix_bind(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };

    /* sun_path is not large -- 104 bytes on the BSDs, 108 on Linux --
     * and a path that does not fit would otherwise be bound truncated,
     * which is a socket at an address nobody sends to. */
    if (strlen(path) >= sizeof(addr.sun_path)) {
	LOG("socket path is too long (%zu >= %zu): %s",
	    strlen(path), sizeof(addr.sun_path), path);
	errno = ENAMETOOLONG;
	return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    /* Clear the way, but only of a socket, and only of a dead one. */
    struct stat st;
    if (stat(path, &st) == 0) {
	if (! S_ISSOCK(st.st_mode)) {
	    LOG("%s exists and is not a socket; refusing to remove it", path);
	    errno = EEXIST;
	    return -1;
	}
	if (someone_is_bound(path)) {
	    LOG("%s is already in use -- another consumer is bound to it",
		path);
	    errno = EADDRINUSE;
	    return -1;
	}
	if ((unlink(path) < 0) && (errno != ENOENT)) {
	    LOG_ERRNO("cannot remove the stale socket %s", path);
	    return -1;
	}
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
	LOG_ERRNO("cannot create the datagram socket");
	return -1;
    }

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
	LOG_ERRNO("cannot bind %s", path);
	close(fd);
	return -1;
    }

    /* After bind, because the file does not exist until then. Not fatal:
     * a socket with the umask's permissions still works for a producer
     * running as the same user, which is every one of them today. */
    if (chmod(path, DGRAM_MODE) < 0)
	LOG_ERRNO("cannot set the mode on %s", path);

    unix_fd = fd;
    return 0;
}


/*
 * Everything one datagram had to say.
 */
static void
apply(const char *data, size_t len)
{
    struct lineproto_reading r[LINEPROTO_READINGS_MAX];
    bool   ok = false;

    size_t n = lineproto_parse(data, len, r, LINEPROTO_READINGS_MAX, &ok);
    if (! ok) {
	/* Logged, not counted: a producer that has gone wrong is worth
	 * seeing, and a display is not the place to keep statistics. */
	LOG("datagram not understood (%zu bytes)", len);
	return;
    }

    for (size_t i = 0 ; i < n ; i++) {
	switch (r[i].kind) {
	case LINEPROTO_INDEX:
	    model_set_index(r[i].litres);
	    break;
	case LINEPROTO_PULSE:
	    model_set_pulse(r[i].count);
	    break;
	case LINEPROTO_TEMPERATURE:
	    model_set_temperature(r[i].celsius);
	    break;
	case LINEPROTO_VALVE:
	    model_set_valve(r[i].closed);
	    break;
	case LINEPROTO_FAILURE:
	    /* The model has nowhere to put this: a reading that failed
	     * leaves the last good one on the screen, ageing, which is
	     * what a dash-when-stale already says. Worth logging. */
	    LOG("%s reports a failure: %s", r[i].measurement, r[i].failure);
	    break;
	case LINEPROTO_IGNORED:
	    break;
	}
    }
}


static void *
unix_loop(void *arg)
{
    (void)arg;

    for (;;) {
	char    buf[DGRAM_MAX];
	ssize_t n = recv(unix_fd, buf, sizeof(buf), 0);

	if (n < 0) {
	    if (errno == EINTR)
		continue;
	    /* Not a spin: whatever this is will not fix itself by being
	     * retried in a tight loop. The thread ends, and the display
	     * carries on with whatever other sources it has. */
	    LOG_ERRNO("the datagram socket stopped being readable");
	    break;
	}
	if (n == 0)
	    continue;			/* an empty datagram says nothing */

	apply(buf, (size_t)n);
    }
    return NULL;
}


int
source_unix_start(void)
{
    const char *path = dgram_path();

    if (unix_bind(path) < 0)
	return -1;

    int rc = pthread_create(&unix_thread, NULL, unix_loop, NULL);
    if (rc != 0) {
	errno = rc;
	LOG_ERRNO("failed to start the datagram thread");
	close(unix_fd);
	unix_fd = -1;
	unlink(path);
	return -1;
    }

    /* Nothing ever joins it: it runs for the life of the program, as the
     * upsd poller does, and the program ends by exiting. */
    pthread_detach(unix_thread);

    LOG("local socket        : %s", path);
    return 0;
}


void
source_unix_stop(void)
{
    if (unix_fd < 0)
	return;

    /* The socket file outlives the process otherwise, and the next run
     * would find it and have to decide whether it was stale. Removing it
     * here is what makes that the unusual case rather than the normal
     * one. The thread is not stopped: the program is on its way out, and
     * the fd closing is what ends its recv(). */
    unlink(dgram_path());
    close(unix_fd);
    unix_fd = -1;
}
