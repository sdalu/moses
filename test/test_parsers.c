/*
 * Unit tests for the option parsers in common.c.
 *
 * These are pure functions with no I/O. parse_gpio's "rpi:" path probes
 * /dev/gpiochip*, so it is exercised here only with an explicit chip name,
 * keeping the tests host-independent (they pass on a CI runner that is not
 * a Raspberry Pi).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
	    failures++;							\
	    fprintf(stderr, "FAIL %s:%d: %s\n",				\
		    __FILE__, __LINE__, #cond);				\
	}								\
    } while (0)


static void
test_mbus_baudrate(void)
{
    long v = 0;
    CHECK(parse_mbus_baudrate("2400",  &v) == 0 && v == 2400);
    CHECK(parse_mbus_baudrate("38400", &v) == 0 && v == 38400);
    CHECK(parse_mbus_baudrate("1234",  &v) <  0);   // not a standard rate
    CHECK(parse_mbus_baudrate("",      &v) <  0);
    CHECK(parse_mbus_baudrate("2400x", &v) <  0);
}

static void
test_s_period(void)
{
    uint64_t v = 0;
    CHECK(parse_s_period("5",    &v) == 0 && v == 5);
    CHECK(parse_s_period("5s",   &v) == 0 && v == 5);
    CHECK(parse_s_period("2min", &v) == 0 && v == 120);
    CHECK(parse_s_period("1h",   &v) == 0 && v == 3600);
    CHECK(parse_s_period("1d",   &v) == 0 && v == 86400);
    CHECK(parse_s_period("1w",   &v) == 0 && v == 604800);
    CHECK(parse_s_period("bad",  &v) <  0);
    CHECK(parse_s_period("5x",   &v) <  0);
    // UINT64_MAX itself parses, but * 604800 (the "w" factor) overflows
    CHECK(parse_s_period("18446744073709551615w", &v) < 0);
    // value too large for strtoull (ERANGE)
    CHECK(parse_s_period("99999999999999999999999", &v) < 0);
}

static void
test_us_period(void)
{
    uint64_t v = 0;
    CHECK(parse_us_period("5",    &v) == 0 && v == 5);
    CHECK(parse_us_period("5us",  &v) == 0 && v == 5);
    CHECK(parse_us_period("5ms",  &v) == 0 && v == 5000);
    CHECK(parse_us_period("1s",   &v) == 0 && v == 1000000);
    CHECK(parse_us_period("2min", &v) == 0 && v == 120000000);
    CHECK(parse_us_period("1h",   &v) == 0 && v == 3600000000ull);
    CHECK(parse_us_period("nope", &v) <  0);
    // UINT64_MAX itself parses, but * 3600000000 (the "h" factor) overflows
    CHECK(parse_us_period("18446744073709551615h", &v) < 0);
}

static void
test_idle_timeout(void)
{
    unsigned long v = 0;
    CHECK(parse_idle_timeout("1",   &v) == 0 && v == 1);
    CHECK(parse_idle_timeout("10w", &v) == 0 && v == 6048000); // max (10 weeks)
    CHECK(parse_idle_timeout("0",   &v) <  0);                 // must be > 0
    CHECK(parse_idle_timeout("11w", &v) <  0);                 // over max
    CHECK(parse_idle_timeout("bad", &v) <  0);
}

static void
test_gpio(void)
{
    char    *chip = NULL;
    uint32_t pin  = 0;

    CHECK(parse_gpio("gpiochip0:17", &chip, &pin) == 0 &&
	  chip && strcmp(chip, "gpiochip0") == 0 && pin == 17);
    free(chip); chip = NULL;

    CHECK(parse_gpio("noseparator", &chip, &pin) < 0);
    CHECK(parse_gpio("gpiochip0:",  &chip, &pin) < 0);   // missing pin
    CHECK(parse_gpio("gpiochip0:x", &chip, &pin) < 0);   // non-numeric pin
}

static void
test_gpio_flags(void)
{
    uint64_t f = 0;
    CHECK(parse_gpio_edge("rising",   &f) == 0);
    CHECK(parse_gpio_edge("falling",  &f) == 0);
    CHECK(parse_gpio_edge("sideways", &f) <  0);

    f = 0;
    CHECK(parse_gpio_bias("pull-up",   &f) == 0);
    CHECK(parse_gpio_bias("pull-down", &f) == 0);
    CHECK(parse_gpio_bias("nope",      &f) <  0);

    f = 0;
    CHECK(parse_gpio_mode("push-pull",  &f) == 0);
    CHECK(parse_gpio_mode("open-drain", &f) == 0);
    CHECK(parse_gpio_mode("bad",        &f) <  0);

    f = 0;
    CHECK(parse_gpio_active("low",  &f) == 0);
    CHECK(parse_gpio_active("high", &f) == 0);
    CHECK(parse_gpio_active("mid",  &f) <  0);
}

int
main(void)
{
    test_mbus_baudrate();
    test_s_period();
    test_us_period();
    test_idle_timeout();
    test_gpio();
    test_gpio_flags();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
