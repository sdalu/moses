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
 * renders LV_COLOR_FORMAT_RGB888, which is three bytes per pixel laid
 * out blue, green, red -- see lv_color_t in LVGL's lv_color.h. The
 * ST7735 reads those three bytes as red, green, blue when MADCTL's BGR
 * bit is clear and as blue, green, red when it is set, and
 * LV_LCD_FLAG_BGR sets it. So the bytes already line up and nothing is
 * swapped here, unlike the RGB565 path this replaces, which had to be
 * byte-swapped on every frame.
 *
 * If red and blue ever come out exchanged on the glass, LV_LCD_FLAG_BGR
 * is the single bit to reconsider: it was carried over from inky-pi,
 * whose screen on this panel is black and white, and black and white
 * survive an exchange of red and blue without anyone noticing.
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


//== Backend ===========================================================

int
backend_init(void)
{
    if (lcd.initialized) {
	LV_LOG_ERROR("backend already initialized");
	return -1;
    }

    if ((bitters_init()						      < 0) ||
	(bitters_gpio_pin_enable(&lcd_dc,        &lcd_dc_cfg)	      < 0) ||
	(bitters_gpio_pin_enable(&lcd_backlight, &lcd_backlight_cfg)  < 0) ||
	(bitters_spi_enable(&lcd_spi,            &lcd_spi_cfg)	      < 0)) {
	LV_LOG_ERROR("failed to initialize bitters library");
	return -1;
    }

    backend_lvgl_boot();

    lv_display_t *disp =
	lv_st7735_create(LCD_H_RES, LCD_V_RES,
			 LV_LCD_FLAG_BGR | LV_LCD_FLAG_RGB666,
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
