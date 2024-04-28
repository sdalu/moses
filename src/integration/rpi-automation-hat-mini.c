#include <sys/cdefs.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

#include "bitters.h"
#include "bitters/rpi.h"
#include "bitters/gpio.h"
#include "bitters/spi.h"
#include "bitters/i2c.h"

#include "integration.h"

#include "lvgl.h"
#include "src/drivers/display/st7735/lv_st7735.h"

#include "bme280.h"


#ifndef __arraycount
#define __arraycount(__x)       (sizeof(__x) / sizeof(__x[0]))
#endif


//== GPIO / SPI / I2C ==================================================

static bitters_gpio_pin_t lcd_dc        =
    BITTERS_GPIO_PIN_INITIALIZER(BITTERS_RPI_GPIO_CHIP, BITTERS_RPI_P1_21);
static bitters_gpio_pin_t lcd_backlight =
    BITTERS_GPIO_PIN_INITIALIZER(BITTERS_RPI_GPIO_CHIP, BITTERS_RPI_P1_22);
static bitters_spi_t      lcd_spi       =
    BITTERS_SPI_INITIALIZER(BITTERS_RPI_SPI0, 1);
static bitters_i2c_t      rpi_i2c       =
    BITTERS_I2C_INITIALIZER(BITTERS_RPI_I2C1);
  

static struct bitters_gpio_cfg lcd_dc_cfg  = {
    .dir       = BITTERS_GPIO_DIR_OUTPUT,
    .defval    = 1,
    .label     = "lcd-dc",
};

static struct bitters_gpio_cfg lcd_backlight_cfg  = {
    .dir       = BITTERS_GPIO_DIR_OUTPUT,
    .defval    = 1,
    .label     = "lcd-backlight",
};

static struct bitters_spi_cfg lcd_spi_cfg = {
    .mode      = BITTERS_SPI_MODE_0,
    .transfer  = BITTERS_SPI_TRANSFER_MSB,
    .word      = BITTERS_SPI_WORDSIZE(8),
    .speed     = 4000000,
};

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
integration_bme280_init(void)
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




//== LVGL ==============================================================


void
lcd_send_cmd(lv_display_t *disp,
	     const uint8_t *cmd, size_t cmd_size,
	     const uint8_t *param, size_t param_size)
{
    // Command
    {
	bitters_gpio_pin_write(&lcd_dc, 0);
	const struct bitters_spi_transfer xfr[] = {
	    { .tx = cmd, .len = cmd_size }
	};
	if (bitters_spi_transfer(&lcd_spi, xfr, 1) < 0)
	    goto failed;
    }

    // Data
    {
	bitters_gpio_pin_write(&lcd_dc, 1);
	const struct bitters_spi_transfer xfr[] = {
	    { .tx = param, .len = param_size }
	};
	if (bitters_spi_transfer(&lcd_spi, xfr, 1) < 0)
	    goto failed;
    }

    return;
    
 failed:
    LV_LOG_ERROR("sending command");	
    bitters_gpio_pin_write(&lcd_dc, 0);
}


void
lcd_send_color(lv_display_t *disp,
	       const uint8_t *cmd, size_t cmd_size,
	       uint8_t *param, size_t param_size)
{
    if (LV_COLOR_FORMAT_NATIVE == LV_COLOR_FORMAT_RGB565)
	lv_draw_sw_rgb565_swap(param, param_size / 2);
    
    // Command
    {	
	bitters_gpio_pin_write(&lcd_dc, 0);
	const struct bitters_spi_transfer xfr[] = {
	    { .tx = cmd, .len = cmd_size }
	};
	if (bitters_spi_transfer(&lcd_spi, xfr, 1) < 0) {
	    LV_LOG_ERROR("sending color (cmd)");
	    goto failed;
	}
    }

    // Data
    if (param_size > 0) {
	bitters_gpio_pin_write(&lcd_dc, 1);
	const struct bitters_spi_transfer xfr[] = {
	    { .tx = param, .len = param_size }
	};
	if (bitters_spi_transfer(&lcd_spi, xfr, 1) < 0) {
	    LV_LOG_ERROR("sending color (data)");
	    goto failed;
	}
    }

    lv_display_flush_ready(disp);
    return;

 failed: 
    bitters_gpio_pin_write(&lcd_dc, 0);
}




static uint32_t
tick_get_cb(void)
{
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t time_ms;
    time_ms = (tv_now.tv_sec * 1000000 + tv_now.tv_usec) / 1000;
    return time_ms;
}


static int
integration_lvgl_init(void) {
    // Initialize LV
    lv_init();
    
    // Set tick 
    lv_tick_set_cb(tick_get_cb);

    // Create display
    lv_display_t * disp =
	lv_st7735_create(LCD_H_RES, LCD_V_RES, LV_LCD_FLAG_BGR,
			 lcd_send_cmd, lcd_send_color);

    lv_st7735_set_invert(disp, true);
    lv_st7735_set_gap(disp, 1, 26); //162-size/2, 132-size/2

    // Initialize buffers
    uint32_t buf_size = LCD_H_RES * LCD_V_RES  / 10 *
	lv_color_format_get_size(lv_display_get_color_format(disp));
    if (buf_size > 65536)
	buf_size = 65536;

    lv_color_t * buf_1 = lv_malloc(buf_size);
    lv_color_t * buf_2 = lv_malloc(buf_size);

    if ((buf_1 == NULL) || (buf_2 == NULL)) {
	LV_LOG_ERROR("display draw buffer malloc failed");	
	goto failed;
    }
    lv_display_set_buffers(disp, buf_1, buf_2, buf_size,
			   LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Set display properties
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

    // Job's done
    return 0;

 failed:
    free(buf_1);
    free(buf_2);
    return -1;
}



//======================================================================

int integration_init(void) {
    // Initalize bitters library (low level gpio/spi/i2c handling)
    if ((bitters_init()                                               < 0) ||
	(bitters_gpio_pin_enable(&lcd_dc ,        &lcd_dc_cfg       ) < 0) ||
	(bitters_gpio_pin_enable(&lcd_backlight , &lcd_backlight_cfg) < 0) ||
	(bitters_i2c_enable(&rpi_i2c,             &rpi_i2c_cfg      ) < 0) ||
	(bitters_spi_enable(&lcd_spi,             &lcd_spi_cfg      ) < 0)) {
	LV_LOG_ERROR("Failed to initialize bitters library");
	return -1;
    }

    integration_bme280_init();
    integration_lvgl_init();
}
