/*
 * gpio.c -- the Linux GPIO half of the shared code.
 *
 *   - The Raspberry Pi header pin mapping, and resolving the header's
 *     GPIO bank by label rather than by device number.
 *   - gpio_open_line(): claim a single line through the character
 *     device.
 *   - The option parsers that speak in GPIO line flags: `chip:pin`
 *     specifications and the edge, bias, mode and active-level words.
 *
 * Split out of common.c because all of it is <linux/gpio.h> and that
 * header exists on exactly one of the systems this tree is worked on.
 * What is left in common.c -- the MQTT wrapper, the time parsers,
 * sleep_until() -- is portable, so moses_display can be built and run
 * on a development machine with the SDL backend while the daemons that
 * actually touch a pin stay Linux-only.
 *
 * Nothing here changed in the move; it is the same code in a different
 * file, and test/test_parsers.c covers it exactly as before.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <dirent.h>

#include <linux/gpio.h>

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
    LOG("GPIO controller      : %s (fd=%d)", devpath, fd);
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
    LOG("GPIO line            : pin %u (fd=%d)", pin, req->fd);

    return fd;
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
