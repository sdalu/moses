/*
 * Unit tests for the payload parsers -- what moses_display makes of what
 * the other daemons publish.
 *
 * These decide the figures on the panel: the meter index, the pulse
 * count, the temperature, and whether a daemon is thought to be alive.
 * Every one of them is handed a buffer that is not NUL-terminated and a
 * length, and every one of them has to refuse rather than guess -- a
 * dash on the screen says "not known", while a plausible wrong number
 * says something false about the water.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "payload.h"

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

/* The payloads really arrive as a buffer and a length, so that is how
 * they are passed here: LIT() is a string literal without its NUL. */
#define LIT(s)		(s), (sizeof(s) - 1)

#define NEAR(a, b)	(fabs((a) - (b)) < 1e-9)


static void
test_text(void)
{
    char buf[16];

    CHECK(payload_text(LIT("hello"), buf, sizeof(buf)) == true);
    CHECK(strcmp(buf, "hello") == 0);

    /* Exactly filling the buffer, NUL included, is the last size that fits */
    CHECK(payload_text(LIT("123456789012345"), buf, sizeof(buf)) == true);
    CHECK(strcmp(buf, "123456789012345") == 0);
    CHECK(payload_text(LIT("1234567890123456"), buf, sizeof(buf)) == false);

    /* An empty payload is not a value */
    CHECK(payload_text("", 0, buf, sizeof(buf)) == false);
    CHECK(payload_text(NULL, 5, buf, sizeof(buf)) == false);

    /* A NUL inside would make every parser below read a shorter payload
     * that happens to parse -- "12\0xx" must not become 12 */
    CHECK(payload_text("12\0xx", 5, buf, sizeof(buf)) == false);

    /* The length is honoured, not the terminator: the caller's buffer
     * may well have more bytes after the payload */
    CHECK(payload_text("21.4,rest-of-the-frame", 4, buf, sizeof(buf)) == true);
    CHECK(strcmp(buf, "21.4") == 0);
}


static void
test_double(void)
{
    double v;

    /* What moses_watermeter actually publishes on `index` */
    CHECK(payload_double(LIT("213025.000"), &v) == true);
    CHECK(NEAR(v, 213025.0));

    CHECK(payload_double(LIT("0"),     &v) == true);  CHECK(NEAR(v, 0));
    CHECK(payload_double(LIT("-1.5"),  &v) == true);  CHECK(NEAR(v, -1.5));
    CHECK(payload_double(LIT("1e3"),   &v) == true);  CHECK(NEAR(v, 1000));

    /* Trailing whitespace is forgiven; a trailing anything else is not */
    CHECK(payload_double(LIT("42 \r\n"), &v) == true); CHECK(NEAR(v, 42));
    CHECK(payload_double(LIT("42x"),     &v) == false);
    CHECK(payload_double(LIT("42 43"),   &v) == false);
    CHECK(payload_double(LIT("4 2"),     &v) == false);

    /* Nothing numeric at all */
    CHECK(payload_double(LIT("litres"), &v) == false);
    CHECK(payload_double(LIT(""),       &v) == false);
    CHECK(payload_double(LIT(" "),      &v) == false);

    /* strtod() accepts these; the panel must not. A NaN index would be
     * drawn as "nan L" and read as a measurement. */
    CHECK(payload_double(LIT("nan"),  &v) == false);
    CHECK(payload_double(LIT("inf"),  &v) == false);
    CHECK(payload_double(LIT("-inf"), &v) == false);
    CHECK(payload_double(LIT("1e400"), &v) == false);	/* overflows to inf */

    /* A failed parse leaves the caller's value alone, so a bad message
     * cannot disturb the figure already on the screen */
    v = 1234;
    CHECK(payload_double(LIT("rubbish"), &v) == false);
    CHECK(NEAR(v, 1234));
}


static void
test_ulong(void)
{
    unsigned long n;

    CHECK(payload_ulong(LIT("3"), &n) == true);   CHECK(n == 3);
    CHECK(payload_ulong(LIT("0"), &n) == true);   CHECK(n == 0);

    /* A count is whole. "3.7" is not three, it is a broken producer. */
    CHECK(payload_ulong(LIT("3.7"), &n) == false);
    CHECK(payload_ulong(LIT("-1"),  &n) == false);

    /* Well-formed and absurd. Converting this to unsigned long is
     * undefined, so it has to be refused before the cast. */
    CHECK(payload_ulong(LIT("1e30"), &n) == false);
    CHECK(payload_ulong(LIT("nan"),  &n) == false);
}


/*
 * The leak report, as src/watermeter.c publishes it on `leak`: two
 * words and two numbers read out of one flat object.
 */
static void
test_json_leak(void)
{
    static const char leak[] =
	"{ \"level\": \"alert\", \"kind\": \"flow\", \"since\": 1790483260, "
	"\"litres\": 134, \"rate\": 5.60, \"source\": \"index\" }";
    char   w[8];
    double v;

    CHECK(payload_json_string(LIT(leak), "level", w, sizeof(w)) == true);
    CHECK(strcmp(w, "alert") == 0);
    CHECK(payload_json_string(LIT(leak), "kind", w, sizeof(w)) == true);
    CHECK(strcmp(w, "flow") == 0);
    CHECK(payload_json_string(LIT(leak), "source", w, sizeof(w)) == true);
    CHECK(strcmp(w, "index") == 0);
    CHECK(payload_json_number(LIT(leak), "since", &v) == true);
    CHECK(NEAR(v, 1790483260));
    CHECK(payload_json_number(LIT(leak), "rate", &v) == true);
    CHECK(NEAR(v, 5.60));

    /* A number where a string is wanted, and a missing key */
    CHECK(payload_json_string(LIT(leak), "since", w, sizeof(w)) == false);
    CHECK(payload_json_string(LIT(leak), "state", w, sizeof(w)) == false);

    /* A value that does not fit is refused, not truncated: "ale" is
     * not "alert" */
    CHECK(payload_json_string(LIT(leak), "level", w, 4) == false);

    /* An escape means the payload is not from src/watermeter.c */
    static const char escaped[] = "{\"kind\": \"fl\\\"ow\"}";
    CHECK(payload_json_string(LIT(escaped), "kind", w, sizeof(w)) == false);

    /* No closing quote */
    static const char open_q[] = "{\"kind\": \"flow}";
    CHECK(payload_json_string(LIT(open_q), "kind", w, sizeof(w)) == false);
}


/* What the UPS state topic may carry, and the stray it once did. */
static void
test_nut_word(void)
{
    CHECK(payload_nut_word("OL") == true);
    CHECK(payload_nut_word("OL CHRG") == true);
    CHECK(payload_nut_word("OB LB RB") == true);
    CHECK(payload_nut_word("ONLINE") == true);		/* a NOTIFYTYPE */

    CHECK(payload_nut_word("unknown") == false);	/* nut-notify, untold */
    CHECK(payload_nut_word("Online") == false);
    CHECK(payload_nut_word("") == false);
    CHECK(payload_nut_word(" OL") == false);
    CHECK(payload_nut_word("OL ") == false);
    CHECK(payload_nut_word("OL  LB") == false);
    CHECK(payload_nut_word("OL,LB") == false);
    CHECK(payload_nut_word(NULL) == false);
}


static void
test_json(void)
{
    /* Exactly what src/sensors.c publishes */
    static const char sensors[] =
	"{\"temperature\": 21.42, \"pressure\": 98600, \"humidity\": 45.30}";
    double v;

    CHECK(payload_json_number(LIT(sensors), "temperature", &v) == true);
    CHECK(NEAR(v, 21.42));
    CHECK(payload_json_number(LIT(sensors), "pressure", &v) == true);
    CHECK(NEAR(v, 98600));
    CHECK(payload_json_number(LIT(sensors), "humidity", &v) == true);
    CHECK(NEAR(v, 45.30));

    /* A key that is not there */
    CHECK(payload_json_number(LIT(sensors), "altitude", &v) == false);

    /* The key is matched with its quotes, so a prefix of a longer key
     * does not find it -- "temp" must not answer with temperature */
    CHECK(payload_json_number(LIT(sensors), "temp", &v) == false);
    CHECK(payload_json_number(LIT(sensors), "ature", &v) == false);

    /* Members may be reordered or added to without this noticing */
    static const char reordered[] =
	"{\"humidity\":45.3,\"extra\":1,\"temperature\":-3.5,\"pressure\":9}";
    CHECK(payload_json_number(LIT(reordered), "temperature", &v) == true);
    CHECK(NEAR(v, -3.5));

    /* Whitespace around the colon */
    CHECK(payload_json_number(LIT("{ \"temperature\"  :  7 }"),
			      "temperature", &v) == true);
    CHECK(NEAR(v, 7));

    /* Malformed: a key with no colon, and a colon with no number */
    CHECK(payload_json_number(LIT("{\"temperature\" 21}"),
			      "temperature", &v) == false);
    CHECK(payload_json_number(LIT("{\"temperature\": }"),
			      "temperature", &v) == false);
    CHECK(payload_json_number(LIT("{\"temperature\": \"warm\"}"),
			      "temperature", &v) == false);

    /* Not JSON at all */
    CHECK(payload_json_number(LIT("21.4"), "temperature", &v) == false);
    CHECK(payload_json_number(LIT(""),     "temperature", &v) == false);
}


static void
test_online(void)
{
    bool up;

    CHECK(payload_online(LIT("online"), &up) == true);   CHECK(up == true);
    CHECK(payload_online(LIT("ONLINE"), &up) == true);   CHECK(up == true);
    CHECK(payload_online(LIT("Online"), &up) == true);   CHECK(up == true);

    /* The last will, and anything else, is not online */
    CHECK(payload_online(LIT("offline"), &up) == true);  CHECK(up == false);
    CHECK(payload_online(LIT("rubbish"), &up) == true);  CHECK(up == false);

    /* "online" with the length cut short is "onlin", which is not it */
    CHECK(payload_online("online", 5, &up) == true);     CHECK(up == false);

    /* No payload is no answer at all, as distinct from "offline" */
    CHECK(payload_online("", 0, &up) == false);
}


int
main(void)
{
    test_text();
    test_double();
    test_ulong();
    test_json();
    test_json_leak();
    test_nut_word();
    test_online();

    printf("%s: %d checks, %d failures\n",
	   (failures == 0) ? "PASS" : "FAIL", checks, failures);
    return (failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
