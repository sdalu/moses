#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <getopt.h>
#include <libgen.h>

#include "bitters.h"
#include "bitters/rpi.h"
#include "bitters/i2c.h"

#include "bme280.h"

#include "common.h"



//== Helpers ===========================================================

static inline float
sea_level_pressure(float pressure, float temperature, float altitude) {
    return pressure * pow(1 - (0.0065 * altitude) /
			      (temperature + 0.0065 * altitude + 273.15),
			  -5.257);
}


//== I2C ===============================================================

static bitters_i2c_t      rpi_i2c       =
    BITTERS_I2C_INITIALIZER(BITTERS_RPI_I2C1);
  
static struct bitters_i2c_cfg rpi_i2c_cfg = {
    .speed     = 0,
};



//== BME280 ============================================================


struct bme280_i2c {
    bitters_i2c_t      *dev;
    bitters_i2c_addr_t  addr;
};


static void
bme280_delay_us(uint32_t period, void *ptr)
{
    usleep(period);
}

static int
bme280_i2c_read(uint8_t reg, uint8_t *data, uint32_t len, void *ptr)
{
    const struct bme280_i2c *i2c = ptr;
    const struct bitters_i2c_transfert xfr[] = {
        { .buf = &reg, .len = sizeof(reg), .write = 1, },
        { .buf = data, .len = len,         .read  = 1, },
    };
    int rc = bitters_i2c_transfert(i2c->dev, i2c->addr,
                                   xfr, __arraycount(xfr));
    return rc < 0 ? rc : 0;
}

static int
bme280_i2c_write(uint8_t reg, const uint8_t *data, uint32_t len, void *ptr)
{
    uint8_t buf[len + 1];         // Doesn't seem possible to split 
    buf[0] =  reg ;               // the write request in two ?!
    memcpy(buf + 1, data, len);   //   -> Use a temporary buffer
    
    const struct bme280_i2c *i2c = ptr;
    const struct bitters_i2c_transfert xfr[] = {
        { .buf = buf, .len =  len + 1, .write = 1, },
    };
    int rc = bitters_i2c_transfert(i2c->dev, i2c->addr,
                                  xfr, __arraycount(xfr));
    return rc < 0 ? rc : 0;
}

//======================================================================

static struct {
    struct bme280_dev       dev;
    struct bme280_settings  settings;
    uint32_t                measurement_delay;
    struct bme280_i2c       i2c;
    bool                    initialized;
} bme280 = {
    .dev = {
	.intf     = BME280_I2C_INTF,
	.intf_ptr = &bme280.i2c,
	.read     = bme280_i2c_read,
	.write    = bme280_i2c_write,
	.delay_us = bme280_delay_us,
    },
    .i2c = {
	.dev  = &rpi_i2c,
	.addr = BITTERS_I2C_ADDR_8 | BME280_I2C_ADDR_PRIM,
    },
};



static int
sensors_bme280_init(void)
{
    // Init bme280
    if (bme280_init(&bme280.dev) != BME280_OK)
	return -1;

    // Always read the current settings before writing
    // especially when all the configuration is not modified
    if (bme280_get_sensor_settings(&bme280.settings, &bme280.dev) != BME280_OK)
	return -1;

    // Configuring the over-sampling rate, filter coefficient and standby time
    bme280.settings.filter       = BME280_FILTER_COEFF_2;
    bme280.settings.osr_h        = BME280_OVERSAMPLING_2X;
    bme280.settings.osr_p        = BME280_OVERSAMPLING_2X;
    bme280.settings.osr_t        = BME280_OVERSAMPLING_2X;
    bme280.settings.standby_time = BME280_STANDBY_TIME_0_5_MS;

    // Save settings
    if (bme280_set_sensor_settings(BME280_SEL_ALL_SETTINGS,
				   &bme280.settings, &bme280.dev) != BME280_OK)
	return -1;

    // Calculate measurement time in microseconds
    if (bme280_cal_meas_delay(&bme280.measurement_delay,
			      &bme280.settings) != BME280_OK)
	return -1;

    // Always set the power mode after setting the configuration
    if (bme280_set_sensor_mode(BME280_POWERMODE_NORMAL,
			       &bme280.dev) != BME280_OK)
	return -1;

    // Done
    bme280.initialized = true;
    return 0;
}


int
get_tph(float *temperature, float *pressure, float *humidity)
{
    // Sanity check
    if (! bme280.initialized)
	return -1;
    
    // Check status and wait for measurement delay if necessary
    uint8_t status;
    if (bme280_get_regs(BME280_REG_STATUS, &status, sizeof(status),
			&bme280.dev) != BME280_OK) {
	return -1;
    }

    // Is measuring being done?
    if (status & BME280_STATUS_MEAS_DONE) {
	bme280.dev.delay_us(bme280.measurement_delay, bme280.dev.intf_ptr);
    }
    
    // Read compensated data
    struct bme280_data comp_data;
    if (bme280_get_sensor_data(BME280_ALL, &comp_data,
			       &bme280.dev) != BME280_OK) {
	return -1;
    }

    // Get data
    if (temperature) *temperature = comp_data.temperature;
    if (pressure   ) *pressure    = comp_data.pressure;
    if (humidity   ) *humidity    = comp_data.humidity;

    // Job's done
    return 0;
}


//======================================================================

int sensors_init(void) {
    // Initalize bitters library (low level gpio/spi/i2c handling)
    if ((bitters_init()                             < 0) ||
	(bitters_i2c_enable(&rpi_i2c, &rpi_i2c_cfg) < 0)) {
	return -1;
    }

    // Initialize various sensors
    if (sensors_bme280_init() < 0)
	return -1;

    // Done
    return 0;
}


//======================================================================

char *__progname = "??";



int main(int argc, char *argv[]) {
    char             *progname = basename(argv[0]);
    unsigned long int interval = 60;
    float             altitude = NAN;
    
    // Argument parsing
    static const char *const shortopts = "+i:a:";

    struct option longopts[] = {
	{ "interval",   required_argument, NULL, 'i' },
	{ "altitude",   required_argument, NULL, 'a' },
	{ 0 }
    };

    int opti, optc;
    char *endptr;

    for (;;) {
	optc = getopt_long(argc, argv, shortopts, longopts, &opti);
	if (optc < 0)
	    break;

	switch (optc) {
	case 'i':
	    if (parse_idle_timeout(optarg, &interval) < 0)
		USAGE_DIE("invalid reporting interval (1s .. 10w)");
	    break;
	case 'a':
	    altitude = strtof(optarg, &endptr);
	    if ((*optarg == '\0') || (*endptr != '\0'))
		USAGE_DIE("invalid number for altitude");
	    break;
	default:
	    fprintf(stderr,
		    "usage: %s [--interval=SECONDS] [--altitude=METERS]\n",
		    progname);
	    exit(1);
	}
    }
    argc -= optind;
    argv += optind;
    
    // Initialization
    if (sensors_init() < 0) {
	DIE(2, "Failed to initialized sensors");
    };
    
    // Polling
    struct timespec next_polling, ts;
    clock_gettime(CLOCK_REALTIME, &next_polling);
    while (1) {
	// Environment (Temperature, Pressure, Humidity)
	float temperature, pressure, humidity;
	if (get_tph(&temperature, &pressure, &humidity) < 0) {
	    PUT_FAIL("environment", "read");
	} else {
	    if (! isnan(altitude))
		pressure = sea_level_pressure(pressure, temperature, altitude);
	    PUT_DATA("environment", 
		     "temperature=%0.2f,pressure=%0.0f,humidity=%0.2f",
		     temperature, pressure, humidity);
	}
	
	// Next	 
	next_polling.tv_sec += interval;
    sleep_again:
	if (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME,
			    &next_polling, NULL) < 0) {
	    assert(errno == EINTR);
	    goto sleep_again;
	}
    }
}
