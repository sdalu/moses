#ifndef __COMMON_H
#define __COMMON_H


/************************************************************************
 * Helpers                                                              *
 ************************************************************************/

#ifndef __arraycount
#define __arraycount(__x)       (sizeof(__x) / sizeof(__x[0]))
#endif


/************************************************************************
 * Log and debug                                                        *
 ************************************************************************/

#ifndef WITH_LOG
#define LOG(x, ...)
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

#ifndef ASSERT
#include <assert.h>
#define ASSERT(x)							\
    assert(x)
#endif



/************************************************************************
 * Output                                                               *
 ************************************************************************/

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
	fprintf(stdout, "%s failure=%s %lld%09ld\n"			\
		type, failure, (long long)ts.tv_sec, ts.tv_nsec);	\
	PUT_UNLOCK()							\
	errno = errno_saved;						\
    } while(0)
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

#define DIE(code, fmt, ...)						\
    do {								\
	fprintf(stderr, "%s" fmt "\n" , __progname			\
		__VA_OPT__(,) __VA_ARGS__);				\
	exit(code);							\
    } while(0)



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

void reduced_lattency(void);

#endif
