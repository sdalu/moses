/*
 * Unit tests for breaker_parse_state -- the valve command parser.
 *
 * It interprets the payloads received on the MQTT setter topic, so a wrong
 * answer here means misreading an open/close command for the water main.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "breaker_state.h"

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

// Parse a NUL-terminated string by its length.
#define PARSE(s) breaker_parse_state((s), (int)strlen(s))

int
main(void)
{
    // Recognized "on" forms -> 1 (case insensitive)
    CHECK(PARSE("1")    == 1);
    CHECK(PARSE("on")   == 1);
    CHECK(PARSE("true") == 1);
    CHECK(PARSE("ON")   == 1);
    CHECK(PARSE("True") == 1);

    // Recognized "off" forms -> 0
    CHECK(PARSE("0")     == 0);
    CHECK(PARSE("off")   == 0);
    CHECK(PARSE("false") == 0);
    CHECK(PARSE("OFF")   == 0);

    // Unrecognized -> -1
    CHECK(PARSE("")        == -1);
    CHECK(PARSE("2")       == -1);
    CHECK(PARSE("onn")     == -1);
    CHECK(PARSE("o")       == -1);
    CHECK(PARSE(" on")     == -1);   // no trimming
    CHECK(PARSE("on ")     == -1);
    CHECK(PARSE("garbage") == -1);

    // Length is honoured: "on" truncated to 1 char is "o", not a match
    CHECK(breaker_parse_state("on", 1) == -1);
    // Embedded NUL is rejected
    CHECK(breaker_parse_state("o\0n", 3) == -1);
    // Negative length is rejected
    CHECK(breaker_parse_state("on", -1) == -1);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
