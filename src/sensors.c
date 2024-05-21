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



//== BME280 compatibility layer ========================================

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



//== Structures ========================================================

struct sensors_bme280 {
    struct bme280_dev       dev;
    struct bme280_settings  settings;
    struct bme280_i2c       i2c;
    uint32_t                measurement_delay;
    bool                    initialized;
};

struct sensors_mqtt {
    struct mqtt handler;
    struct {
	char *sensors;
	char *error;
    } topic;
};

struct sensors {
    struct sensors_mqtt   mqtt;
    struct sensors_bme280 bme280;
    int                   reduced_latency;
    unsigned long int     interval;
    float                 altitude;
};



//== Global context ====================================================

char *__progname = "??";

struct sensors sensors = {
    .mqtt     = {
	.handler         = MQTT_INITIALIZER(),
	.topic.sensors   = "sensors",
	.topic.error     = "error",
    },
    .bme280   = {
	.dev = {
	    .intf     = BME280_I2C_INTF,
	    .intf_ptr = &sensors.bme280.i2c,
	    .read     = bme280_i2c_read,
	    .write    = bme280_i2c_write,
	    .delay_us = bme280_delay_us,
	},
	.i2c = {
	    .dev  = &rpi_i2c,
	    .addr = BITTERS_I2C_ADDR_8 | BME280_I2C_ADDR_PRIM,
	},
    },
    .interval = 60,
    .altitude = NAN,
};



//== MQTT ==============================================================

int
sensors_mqtt_init(struct sensors_mqtt *mqtt)
{
    int rc;
    
    // Adjust prefix
    char *prefix = getenv("MQTT_PREFIX");
    if (prefix == NULL)
	prefix = MQTT_PREFIX;
    MQTT_ADJUST_TOPIC(mqtt, sensors, prefix);
    MQTT_ADJUST_TOPIC(mqtt, error,   prefix);
    
    LOG("MQTT sensors         : %s", mqtt->topic.sensors);
    LOG("MQTT error reporting : %s", mqtt->topic.error);

    // Initialise (0 = not enabled)
    rc = mqtt_init(&mqtt->handler, 0, NULL);
    if (rc <= 0) return rc;
    
    // Start
    rc = mqtt_start(&mqtt->handler);
    if (rc < 0) goto failed;

    // Done
    LOG("MQTT connection established");
    return 0;
    
 failed:
    mqtt_destroy(&mqtt->handler);
    return -1;
}



//== BME280 ============================================================

static int
sensors_bme280_init(struct sensors_bme280 *bme280)
{
    // Init bme280
    if (bme280_init(&bme280->dev) != BME280_OK)
	return -1;

    // Always read the current settings before writing
    // especially when all the configuration is not modified
    if (bme280_get_sensor_settings(&bme280->settings,
				   &bme280->dev) != BME280_OK)
	return -1;

    // Configuring the over-sampling rate, filter coefficient and standby time
    bme280->settings.filter       = BME280_FILTER_COEFF_2;
    bme280->settings.osr_h        = BME280_OVERSAMPLING_2X;
    bme280->settings.osr_p        = BME280_OVERSAMPLING_2X;
    bme280->settings.osr_t        = BME280_OVERSAMPLING_2X;
    bme280->settings.standby_time = BME280_STANDBY_TIME_0_5_MS;

    // Save settings
    if (bme280_set_sensor_settings(BME280_SEL_ALL_SETTINGS,
				   &bme280->settings,
				   &bme280->dev) != BME280_OK)
	return -1;

    // Calculate measurement time in microseconds
    if (bme280_cal_meas_delay(&bme280->measurement_delay,
			      &bme280->settings) != BME280_OK)
	return -1;

    // Always set the power mode after setting the configuration
    if (bme280_set_sensor_mode(BME280_POWERMODE_NORMAL,
			       &bme280->dev) != BME280_OK)
	return -1;

    // Done
    bme280->initialized = true;
    return 0;
}


int
sensors_get_tph(struct sensors *sensors,
		float *temperature, float *pressure, float *humidity)
{
    struct sensors_bme280 *bme280 = &sensors->bme280;
    
    // Sanity check
    if (! bme280->initialized)
	return -1;
    
    // Check status and wait for measurement delay if necessary
    uint8_t status;
    if (bme280_get_regs(BME280_REG_STATUS, &status, sizeof(status),
			&bme280->dev) != BME280_OK) {
	return -1;
    }

    // Is measuring being done?
    if (status & BME280_STATUS_MEAS_DONE) {
	bme280->dev.delay_us(bme280->measurement_delay, bme280->dev.intf_ptr);
    }
    
    // Read compensated data
    struct bme280_data comp_data;
    if (bme280_get_sensor_data(BME280_ALL, &comp_data,
			       &bme280->dev) != BME280_OK) {
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

    // Initialize sub-components
    if ((sensors_mqtt_init(&sensors.mqtt)     < 0) ||
	(sensors_bme280_init(&sensors.bme280) < 0))
	return -1;

    // Done
    return 0;
}



//======================================================================

static void
sensors_parse_config(int argc, char **argv, struct sensors *s)
{
    // Argument parsing
    static const char *const shortopts = "+ri:a:h";

    struct option longopts[] = {
	{ "reduced-latency", no_argument,	NULL, 'r' },
	{ "interval",        required_argument, NULL, 'i' },
	{ "altitude",        required_argument, NULL, 'a' },
	{ NULL }
    };

    int opti, optc;
    char *endptr;

    for (;;) {
	optc = getopt_long(argc, argv, shortopts, longopts, &opti);
	if (optc < 0)
	    break;

	switch (optc) {
	case 'r':
	    s->reduced_latency = 1;
	    break;
	case 'i':
	    if (parse_idle_timeout(optarg, &s->interval) < 0)
		USAGE_DIE("invalid reporting interval (1s .. 10w)");
	    break;
	case 'a':
	    s->altitude = strtof(optarg, &endptr);
	    if ((*optarg == '\0') || (*endptr != '\0'))
		USAGE_DIE("invalid number for altitude");
	    break;
	case 'h':
	    printf("%s [opts]\n", __progname);
	    printf("  -r, --reduced-latency     try to reduce latency\n");
	    printf("  -i, --interval=SEC        publish sensors information every SEC\n");
	    printf("  -a, --altitude=METERS     compute sea level pressure\n");
	    printf("\n");
	    exit(1);
	default:
	    exit(1);
	}
    }
    argc -= optind;
    argv += optind;
}


int main(int argc, char *argv[]) {
    __progname = basename(argv[0]);

    // Shortcuts
    struct sensors      *s    = &sensors;
    struct sensors_mqtt *mqtt = &sensors.mqtt;
        
    // Configuration
    mqtt_config_from_env(&sensors.mqtt.handler);
    sensors_parse_config(argc, argv, &sensors);
    
    // Initialization
    if (sensors_init() < 0) 
	DIE(2, "Failed to initialized");
    
    // Reducing latency
    if (sensors.reduced_latency)
	reduced_lattency();

    // Polling
    struct timespec next_polling;
    clock_gettime(CLOCK_REALTIME, &next_polling);
    while (1) {
	// Environment (Temperature, Pressure, Humidity)
	float temperature, pressure, humidity;
	if (sensors_get_tph(s, &temperature, &pressure, &humidity) < 0) {
	    PUT_FAIL("environment", "read");

	    static char *msg =
		MQTT_ERROR_MSG("breaker", "error",
			       "failed to read sensors values");
	    MQTT_PUBLISH(mqtt, sensors, 1, false, msg);
	} else {
	    if (! isnan(s->altitude))
		pressure = sea_level_pressure(pressure, temperature,
					      s->altitude);
	    PUT_DATA("environment", 
		     "temperature=%0.2f,pressure=%0.0f,humidity=%0.2f",
		     temperature, pressure, humidity);

	    static char *fmt = 
		     "{" "\"temperature\"" ": %0.2f" ", "
		         "\"pressure\""    ": %0.0f" ", "
		         "\"humidity\""    ": %0.2f"
		    "}";
	    MQTT_PUBLISH(mqtt, sensors, 1, false,
			 fmt, temperature, pressure, humidity);
	}
	
	// Next	 
	next_polling.tv_sec += s->interval;
    sleep_again:
	if (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME,
			    &next_polling, NULL) < 0) {
	    assert(errno == EINTR);
	    goto sleep_again;
	}
    }
}
