/*
 * moses_display -- Pimoroni Automation HAT Mini backend
 *
 * The HAT carries a 0.96" 160x80 ST7735 LCD, wired as:
 *
 *   SPI0 CE1        P1_26 / GPIO 7   (driven by the spidev device)
 *   data/command    P1_21 / GPIO 9
 *   backlight       P1_22 / GPIO 25
 *
 * The controller is portrait (80x160) and the panel is mounted
 * landscape, hence the rotation and the RAM offsets below.
 *
 * Only the display is handled here. The rest of the HAT -- the ADS1015
 * analog inputs, the three buffered inputs, the three sinking outputs
 * and the relay -- is untouched, and its pins are left alone. That
 * matters on moses: the relay on this same HAT is the solenoid valve,
 * held by moses_breaker, and nothing here may go near it.
 *
 * Ported from the inky-pi tree (src/backend/rpi-automation-hat-mini.c),
 * itself descended from the experimental src/integration/ this replaces.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitters.h"
#include "bitters/rpi.h"
#include "bitters/gpio.h"
#include "bitters/spi.h"

#include "lvgl.h"

#include "backend.h"
#include "internal.h"


/* Resolution of the controller, before rotation. */
#define LCD_H_RES		80
#define LCD_V_RES		160

/* Offset of pixel (0,0) in the controller's VRAM. The ST7735 has room
 * for 132x162 and this panel sits at (1,26) in it. */
#define LCD_GAP_X		1
#define LCD_GAP_Y		26

/* A tenth of the screen, which is what LVGL suggests for partial
 * rendering, capped so the two buffers stay modest. Sized from the
 * colour format at run time, so the move to three bytes per pixel is
 * already accounted for: a tenth of 160x80 in RGB888 is 3840 bytes. */
#define LCD_BUF_FRACTION	10
#define LCD_BUF_MAX		65536


//== GPIO / SPI ========================================================

static bitters_gpio_pin_t lcd_dc =
    BITTERS_GPIO_PIN_INITIALIZER(BITTERS_RPI_GPIO_CHIP, BITTERS_RPI_P1_21);
static bitters_gpio_pin_t lcd_backlight =
    BITTERS_GPIO_PIN_INITIALIZER(BITTERS_RPI_GPIO_CHIP, BITTERS_RPI_P1_22);
static bitters_spi_t lcd_spi =
    BITTERS_SPI_INITIALIZER(BITTERS_RPI_SPI0, 1);

static bitters_gpio_cfg_t lcd_dc_cfg = {
    .dir	= BITTERS_GPIO_DIR_OUTPUT,
    .defval	= 1,
    .label	= "lcd-dc",
};

static bitters_gpio_cfg_t lcd_backlight_cfg = {
    .dir	= BITTERS_GPIO_DIR_OUTPUT,
    .defval	= 1,
    .label	= "lcd-backlight",
};

static bitters_spi_cfg_t lcd_spi_cfg = {
    .mode	= BITTERS_SPI_MODE_0,
    .transfer	= BITTERS_SPI_TRANSFER_MSB,
    .word	= BITTERS_SPI_WORDSIZE(8),
    .speed	= 4000000,
};


//== State =============================================================

static struct {
    lv_display_t       *disp;
    lv_color_t         *buf_1;
    lv_color_t         *buf_2;
    struct backend_info info;
    bool                initialized;
} lcd = {
    .info = {
	.name	= "Automation HAT Mini (ST7735)",
    },
};


//== LVGL <-> ST7735 ===================================================

/* One SPI transfer, with data/command already selected. The tx buffer
 * of bitters_spi_transfer is not const -- nothing writes through it on
 * a write-only transfer, so the cast is safe. */
static int
lcd_write(const uint8_t *buf, size_t len)
{
    if (len == 0)
	return 0;

    const struct bitters_spi_transfer xfr[] = {
	{ .tx = (uint8_t *)buf, .len = len },
    };
    return bitters_spi_transfer(&lcd_spi, xfr, 1);
}


static void
lcd_send_cmd(lv_display_t *disp,
	     const uint8_t *cmd, size_t cmd_size,
	     const uint8_t *param, size_t param_size)
{
    LV_UNUSED(disp);

    bitters_gpio_pin_write(&lcd_dc, 0);
    if (lcd_write(cmd, cmd_size) < 0)
	goto failed;

    bitters_gpio_pin_write(&lcd_dc, 1);
    if (lcd_write(param, param_size) < 0)
	goto failed;

    return;

 failed:
    LV_LOG_ERROR("sending command");
    bitters_gpio_pin_write(&lcd_dc, 0);
}


/*
 * Pixels, in the order the controller expects them.
 *
 * The panel is put in 18-bit mode (LV_LCD_FLAG_RGB666 below) and LVGL
 * renders LV_COLOR_FORMAT_RGB888, three bytes per pixel laid out blue,
 * green, red -- see lv_color_t in LVGL's lv_color.h. Nothing is swapped
 * here, unlike the RGB565 path this replaces, which had to be
 * byte-swapped on every frame.
 *
 * LV_LCD_FLAG_BGR is deliberately NOT set, and that is worth recording
 * because it looks wrong. MADCTL's BGR bit decides which way round the
 * controller reads a pixel's three bytes, and reasoning from LVGL's
 * memory order says to set it. On this panel that produces red and blue
 * exchanged: the cyan water drop of the dashboard came out gold, which
 * is exactly 0x3FC7F4 read backwards. Clear is correct here, measured
 * rather than derived.
 *
 * It went unnoticed for as long as it did because inky-pi, which this
 * backend comes from, draws black and white on this panel -- and black
 * and white survive an exchange of red and blue with nothing to show
 * for it. This tree is the first to send it a colour.
 */
static void
lcd_send_color(lv_display_t *disp,
	       const uint8_t *cmd, size_t cmd_size,
	       uint8_t *param, size_t param_size)
{
    LV_UNUSED(disp);

    bitters_gpio_pin_write(&lcd_dc, 0);
    if (lcd_write(cmd, cmd_size) < 0) {
	LV_LOG_ERROR("sending color (cmd)");
	goto failed;
    }

    bitters_gpio_pin_write(&lcd_dc, 1);
    if (lcd_write(param, param_size) < 0) {
	LV_LOG_ERROR("sending color (data)");
	goto failed;
    }

    lv_display_flush_ready(disp);
    return;

 failed:
    bitters_gpio_pin_write(&lcd_dc, 0);
    /* Still hand the buffer back: LVGL waits for this and a display
     * that stops refreshing hides every later error. */
    lv_display_flush_ready(disp);
}


/*
 * What spidev will carry in one ioctl, or 0 when it cannot be asked.
 *
 * The module's default is a single page, and a frame is larger than
 * that, so a panel on a machine that was never told otherwise fails
 * halfway through a transfer rather than at startup. Asking costs one
 * small read and turns that into a sentence naming the fix.
 */
static unsigned long
spidev_bufsiz(void)
{
    static const char path[] = "/sys/module/spidev/parameters/bufsiz";
    unsigned long     value  = 0;
    FILE             *f      = fopen(path, "re");

    if (f == NULL)
	return 0;
    if (fscanf(f, "%lu", &value) != 1)
	value = 0;
    fclose(f);
    return value;
}


//== Backend ===========================================================

int
backend_init(void)
{
    if (lcd.initialized) {
	LV_LOG_ERROR("backend already initialized");
	return -1;
    }

    /* One call per message. These four fail for unrelated reasons --
     * a missing subsystem, a pin another driver owns, a spidev that
     * is not there -- and a single message for all of them sends the
     * reader to the library when the kernel was refusing a pin.
     */
    if (bitters_init() < 0) {
	LV_LOG_ERROR("bitters_init failed");
	return -1;
    }

    /* The data/command line is GPIO 9, which is SPI0's MISO as well:
     * bitters/rpi.h defines BITTERS_RPI_P1_21 and BITTERS_RPI_SPI0_MISO
     * as the same pin. Under a plain `dtparam=spi=on` the SPI driver
     * owns it and the claim is refused; `dtoverlay=spi0-2cs,no_miso`
     * frees it (docs/hardware.md, *LCD*). Spelled out here because the
     * kernel says so only in dmesg, and because a kernel that allows
     * the overlap does not make the pin free -- Linux 6.18.39 allowed
     * it and 6.18.50 refuses, on a device tree that did not change.
     */
    if (bitters_gpio_pin_enable(&lcd_dc, &lcd_dc_cfg) < 0) {
	LV_LOG_ERROR("cannot claim the LCD data/command pin (GPIO %d): "
		     "it is SPI0's MISO too, so config.txt wants "
		     "dtoverlay=spi0-2cs,no_miso", lcd_dc.id);
	return -1;
    }

    if (bitters_gpio_pin_enable(&lcd_backlight, &lcd_backlight_cfg) < 0) {
	LV_LOG_ERROR("cannot claim the LCD backlight pin (GPIO %d)",
		     lcd_backlight.id);
	return -1;
    }

    if (bitters_spi_enable(&lcd_spi, &lcd_spi_cfg) < 0) {
	LV_LOG_ERROR("cannot open the LCD's SPI device (/dev/spidev%d.%d)",
		     lcd_spi.id, lcd_spi.ce);
	return -1;
    }

    backend_lvgl_boot();

    lv_display_t *disp =
	lv_st7735_create(LCD_H_RES, LCD_V_RES, LV_LCD_FLAG_RGB666,
			 lcd_send_cmd, lcd_send_color);
    if (disp == NULL) {
	LV_LOG_ERROR("failed to create display");
	return -1;
    }

    lv_st7735_set_invert(disp, true);
    lv_st7735_set_gap(disp, LCD_GAP_X, LCD_GAP_Y);

    uint32_t buf_size = LCD_H_RES * LCD_V_RES / LCD_BUF_FRACTION *
	lv_color_format_get_size(lv_display_get_color_format(disp));
    if (buf_size > LCD_BUF_MAX)
	buf_size = LCD_BUF_MAX;

    /* buf_size is the largest single write lcd_send_color() makes, so it
     * is exactly what spidev has to be willing to carry. Checked here
     * rather than in a script because this is the only place the figure
     * is known: it comes from the panel's size, LCD_BUF_FRACTION and
     * whatever colour format LVGL settled on. A bufsiz of 0 means the
     * file was not there to read, which is not a verdict. */
    unsigned long bufsiz = spidev_bufsiz();
    if ((bufsiz > 0) && (bufsiz < buf_size)) {
	LV_LOG_ERROR("spidev carries %lu bytes and a transfer is %" LV_PRIu32
		     ": add spidev.bufsiz=%" LV_PRIu32
		     " or more to cmdline.txt", bufsiz, buf_size, buf_size);
	goto failed;
    }

    lcd.buf_1 = malloc(buf_size);
    lcd.buf_2 = malloc(buf_size);
    if ((lcd.buf_1 == NULL) || (lcd.buf_2 == NULL)) {
	LV_LOG_ERROR("display draw buffer malloc failed");
	goto failed;
    }
    lv_display_set_buffers(disp, lcd.buf_1, lcd.buf_2, buf_size,
			   LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Panel mounted landscape on a portrait controller. */
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

    lcd.disp          = disp;
    lcd.info.hor_res  = lv_display_get_horizontal_resolution(disp);
    lcd.info.ver_res  = lv_display_get_vertical_resolution(disp);
    lcd.initialized   = true;
    return 0;

 failed:
    free(lcd.buf_1);
    free(lcd.buf_2);
    lcd.buf_1 = lcd.buf_2 = NULL;
    lv_display_delete(disp);
    return -1;
}


void
backend_deinit(void)
{
    if (! lcd.initialized)
	return;

    backend_set_backlight(false);

    lv_display_delete(lcd.disp);
    lcd.disp = NULL;

    free(lcd.buf_1);
    free(lcd.buf_2);
    lcd.buf_1 = lcd.buf_2 = NULL;

    bitters_spi_disable(&lcd_spi);
    bitters_gpio_pin_disable(&lcd_backlight);
    bitters_gpio_pin_disable(&lcd_dc);

    lcd.initialized = false;
}


lv_display_t *
backend_display(void)
{
    return lcd.disp;
}


const struct backend_info *
backend_info(void)
{
    return lcd.initialized ? &lcd.info : NULL;
}


lv_color_t
backend_accent_color(void)
{
    /* A full-colour panel: red is red, with nothing to quantise it to.
     * inky-pi returns black here because the same screen has to stay
     * legible on a two-ink e-paper; this tree has only the LCD, and an
     * alarm nobody can see is worse than no alarm. */
    return lv_color_hex(0xff0000);
}


void
backend_set_backlight(bool on)
{
    bitters_gpio_pin_write(&lcd_backlight, on ? 1 : 0);
}
