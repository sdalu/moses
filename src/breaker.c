#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <linux/gpio.h>
#include <time.h>

#include <getopt.h>
#include <libgen.h>

#include "common.h"



//======================================================================

/* Infered from: uapi/linux/gpio.h */
#define MAX_EVENTS ((GPIO_V2_LINES_MAX) * 16)

#define BUFSIZE   1024


struct breaker_control {
    struct {                     // Controller
	char    *id;             //  - identifier
	int      fd;             //  - file descriptor
    } ctrl;
    struct {                     // Pin
	uint32_t id;             //  - identifier
	int      fd;             //  - file descriptor
	uint64_t flags;          //  - flags
	char    *label;          //  - label
	int      defval;         //  - default value
    } pin;
    unsigned long idle_timeout;  // idle timeout in s
    int      state;              // actual state
};

struct breaker {
    int reduced_latency;
    struct breaker_control control;
};



//======================================================================

int
breaker_init(struct breaker *b)
{
    struct breaker_control *bc = &b->control;

    char *devpath = NULL;
    int   rc      = -EINVAL;
    int   fd      = -1;
    
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
    b->control.state = bc->pin.defval ? 1 : 0;
    
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
	      .attr.values = b->control.state << 0
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
    
    return -1;   
}


int
breaker_set_state(struct breaker *b, int state) {
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

    // Job's done
    if (rc < 0) {
	return -1;
    } else {
	b->control.state = state;
	return 0;
    }
}


int
breaker_get_state(struct breaker *b) {
    return b->control.state;
}



//======================================================================

char *__progname = "??";


static void
breaker_parse_config(int argc, char **argv, struct breaker *b)
{
    struct breaker_control *bc = &b->control;

    static const char *const shortopts = "+rP:L:M:A:I:h";
    
    const struct option longopts[] = {
	{ "reduced-latency", no_argument,	NULL,	'r' },
	{ "pin",             required_argument, NULL,	'P' },
	{ "pin-label",       required_argument, NULL,	'L' },
	{ "mode",            required_argument, NULL,	'M' },
	{ "active",          required_argument, NULL,	'A' },
	{ "idle-timeout",    required_argument, NULL,	'I' },
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
	    printf("pulse-counting [opts]\n");
	    printf("  -r, --reduced-latency            try to reduce latency\n");
	    printf("  -P, --pin=CTRL:PIN               gpio pulse counting pin\n");
	    printf("  -L, --pin-label=STRING           gpio pin label\n");
	    printf("  -M, --mode=as-is|push-pull|      gpio mode\n");
	    printf("             open-drain|open-source\n");
	    printf("  -A, --active=low|high            gpio active state\n");
	    printf("  -I, --idle-timeout=SEC           notify state if no command send\n");
	    exit(0);
	case 0:
	    break;
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




//======================================================================

struct breaker breaker =  {
    .control = {
	.ctrl.id   = NULL,
	.ctrl.fd   = -1,
	.pin.id    = ~0,
	.pin.fd    = -1,
	.pin.flags = 0,
	.pin.label = "breaker-control",
    },
};


int
main(int argc, char **argv)
{
    __progname = basename(argv[0]);

    // Configuration
    breaker_parse_config(argc, argv, &breaker);
    
    // Initialization
    if (breaker_init(&breaker) < 0) {
	DIE(2, "failed to initialize breaker");
    }

    // Reducing latency
    if (breaker.reduced_latency) {
	reduced_lattency();
    }

    char buff[BUFSIZE + 1] = { 0 };
    struct pollfd    pfd   = { .fd = 0, .events = POLLIN | POLLPRI };
    struct timespec  ts    = { .tv_sec = breaker.control.idle_timeout,};
    struct timespec *ts_p  = breaker.control.idle_timeout == 0 ? NULL : &ts;
    
    while(1) {
	int rc = ppoll(&pfd, 1, ts_p, NULL);
	if (rc < 0) {
	    LOG_ERRNO("ppoll failed");
	    continue;
	} else if (rc == 0) {
	    PUT_DATA("breaker", "state=%d", breaker_get_state(&breaker));
	    continue;
	}

	// Read data
	// (buffer is big enough for normal operations)
	ssize_t len = read(0, buff, BUFSIZE);
	if (len <  0) { continue; } // Fail safe, keep going
	if (len == 0) { break;    } // End of file reached

	// Ensure NUL-terminated string at end of line
	size_t idx = strcspn(buff, "\r\n");
	buff[idx] = '\0';

	for (int i = 0 ; buff[i] ; i++)
	    buff[i] = tolower(buff[i]);

	int state = -1;
	if        (!strcmp(buff, "0"    ) ||
		   !strcmp(buff, "false") ||
		   !strcmp(buff, "off"  )) {
	    state =  0;
	} else if (!strcmp(buff, "1"    ) ||
		   !strcmp(buff, "true" ) ||
		   !strcmp(buff, "on"   )) {
	    state =  1;
	} else {
	    LOG("got garbage command");
	    state = -1;
	}
	    
	if (state >= 0) {
	    breaker_set_state(&breaker, state);
	    PUT_DATA("breaker", "state=%d", breaker_get_state(&breaker));
	}	
    }
    
    
    return 0;
}



