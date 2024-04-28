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
#include <errno.h>
#include <linux/gpio.h>
#include <time.h>

#include <getopt.h>
#include <libgen.h>

#include <mbus/mbus.h>

#include "common.h"

//======================================================================

/* Infered from: uapi/linux/gpio.h */
#define MAX_EVENTS ((GPIO_V2_LINES_MAX) * 16)


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

struct watermeter {
    int reduced_latency;
    struct pulse_counting pulse_counting;
    struct index_reader   index_reader;
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
    mbus_data_variable_header *header = &data->header;
    int                        rc     = -1;

#if 0
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



//======================================================================

int
watermeter_init(struct watermeter *w)
{
    struct pulse_counting *pc = &w->pulse_counting;
    struct index_reader   *ir = &w->index_reader;

    
    //
    // M-BUS
    //
    if (ir->device != NULL) {
	ir->mbus = mbus_open(ir->device, ir->baudrate);
	if (ir->mbus == NULL) {
	    LOG("failed to open/connect to m-bus (dev=%s, baudrate=%d)",
		ir->device, ir->baudrate);
	    goto failed_mbus;
	}
	mbus_softreset(ir->mbus);
	LOG("m-bus device %s opened at %d bauds", ir->device, ir->baudrate);
    } else {
	LOG("No m-bus device specified (skipping)");
    }
    
    
    //
    // GPIO
    //
    if ((pc->ctrl.id != NULL) || (pc->pin.id != ~0)) {
	char *devpath = NULL;
	int   rc      = -EINVAL;
	int   fd      = -1;
	
	// Build device path
	rc = asprintf(&devpath, "/dev/%s", pc->ctrl.id);
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
	
	// Get line (with a single gpio)
	struct gpio_v2_line_request req = {
	    .num_lines        = 1,
	    .offsets          = { [0] = pc->pin.id },
	    .config.flags     = GPIO_V2_LINE_FLAG_INPUT | pc->pin.flags,
	    .config.num_attrs = pc->flags.debounce ? 1 : 0,
	    .config.attrs     = {
		{ .mask                    = 1 << 0,
		  .attr.id                 = GPIO_V2_LINE_ATTR_ID_DEBOUNCE,
		  .attr.debounce_period_us = pc->debounce                  }
	    }
	};
	strncpy(req.consumer, pc->pin.label, sizeof(req.consumer));
	
	// Release memory
	free(devpath);
	
	// Save controller file descriptor
	pc->ctrl.fd = fd;
	
	// Call ioctl
	rc = ioctl(pc->ctrl.fd, GPIO_V2_GET_LINE_IOCTL, &req);
	if (rc < 0) {
	    LOG_ERRNO("failed to issue GPIO_V2_GET_LINE IOCTL for pin %d",
		      pc->pin.id);
	    goto failed_gpio;
	}
	
	// Store file descriptor
	pc->pin.fd = req.fd;
	LOG("GPIO line configured as single pin %d (fd=%d)",
	    pc->pin.id, pc->pin.fd);
    } else {
	LOG("GPIO line not defined (skipping)");
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


//======================================================================

char *__progname = "??";


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
	    exit(0);
	case 0:
	    break;
	default:
	    exit(1);
	}
    }
    argc -= optind;
    argv += optind;
}



//======================================================================

struct watermeter watermeter =  {
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
};

static pthread_t thr_pulse_counting;
static pthread_t thr_index_reader;


__attribute__((noreturn))
static void * index_reader_task(void *parameters) {
    struct index_reader   *ir = parameters;

    // Polling
    struct timespec next_polling;
    clock_gettime(INTERVAL_CLOCK, &next_polling);
    while (1) {
	// Watermeter
	double value;
	if (watermeter_get_index(&watermeter, &value) < 0) {
	    PUT_FAIL("watermeter", "read");
	} else {
	    PUT_DATA("watermeter", "index=%0.3f", value);
	}
	
	// Next	 
	next_polling.tv_sec += ir->interval;
    sleep_again:
	if (clock_nanosleep(INTERVAL_CLOCK, TIMER_ABSTIME,
			    &next_polling, NULL) < 0) {
	    assert(errno == EINTR);
	    goto sleep_again;
	}
    }
}


__attribute__((noreturn))
static void * pulse_counting_task(void *parameters) {
    struct pulse_counting *pc = parameters;

    while(1) {	
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
		PUT_DATA("watermeter", "pulse=0");
		continue;
	    }
	}

	struct gpio_v2_line_event event[MAX_EVENTS];
	ssize_t size = read(pc->pin.fd, event, sizeof(event));
	    
	if (size < 0) {
	    LOG_ERRNO("failed to read event");
	    continue;
	} else if (size % sizeof(struct gpio_v2_line_event)) {
	    LOG("got event of unexpected size");
	    continue;
	}

	PUT_DATA("watermeter", "pulse=%d",
		 size / sizeof(struct gpio_v2_line_event));
    }
}


int
main(int argc, char **argv)
{
    __progname = basename(argv[0]);

    // Configuration
    watermeter_parse_config(argc, argv, &watermeter);

    // Initialization
    if (watermeter_init(&watermeter) < 0) {
	DIE(2, "failed to initialize watermeter");
    }

    // Reducing latency
    if (watermeter.reduced_latency) {
	reduced_lattency();
    }
    

    // Starting threads
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



