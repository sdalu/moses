/*
 * moses_display -- the leak banner's icons (src/display/dashboard.c)
 *
 * Five Font Awesome Free 6.7.2 solid glyphs at 10 px, 4 bpp, falling
 * back to Montserrat 10 for everything else, so one label carries an
 * icon and its text on the same baseline -- this font's line height is
 * Montserrat 10's, 11 px, which is why it is 10 and not 11:
 *
 *     U+E50E  house-flood-water   a flow that does not stop
 *     U+E006  faucet-drip         a regular drip
 *     U+E005  faucet              a house that never goes quiet: a tap left running
 *     U+F2DC  snowflake           a room at freezing, beside a leak
 *     U+F06D  fire                a room far too hot, beside a leak
 *
 * Font Awesome Free (https://fontawesome.com): the font file is under
 * the SIL Open Font License 1.1, the icons under CC BY 4.0. Generated,
 * not edited -- to add a glyph, rerun with the new code point added to
 * --range, from the webfonts/ directory of the fontawesome-free npm
 * package:
 *
 *     npx lv_font_conv@1.5.3 --size 10 --bpp 4 --format lvgl \
 *         --no-compress --font fa-solid-900.ttf \
 *         --range 0xE50E,0xE006,0xE005,0xF2DC,0xF06D \
 *         --lv-font-name moses_icons_10 \
 *         --lv-fallback lv_font_montserrat_10 --lv-include lvgl.h \
 *         -o icons.c
 *
 * and put this comment back at the top.
 */

/*******************************************************************************
 * Size: 10 px
 * Bpp: 4
 * Opts: --size 10 --bpp 4 --format lvgl --no-compress --font fa-solid-900.ttf --range 0xE50E,0xE006,0xE005,0xF2DC,0xF06D --lv-font-name moses_icons_10 --lv-fallback lv_font_montserrat_10 --lv-include lvgl.h -o icons.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef MOSES_ICONS_10
#define MOSES_ICONS_10 1
#endif

#if MOSES_ICONS_10

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+E005 "" */
    0x0, 0x0, 0x30, 0x0, 0x0, 0x8, 0xcc, 0xfb,
    0xd4, 0x0, 0x3, 0x43, 0x43, 0x51, 0x0, 0x0,
    0x5, 0xf1, 0x0, 0x0, 0x78, 0xaf, 0xff, 0x96,
    0x10, 0xff, 0xff, 0xff, 0xff, 0xe2, 0xdf, 0xff,
    0xff, 0xff, 0xfb, 0x0, 0x1a, 0xd7, 0x9, 0xfe,
    0x0, 0x0, 0x0, 0x0, 0x32,

    /* U+E006 "" */
    0x0, 0x1, 0x90, 0x10, 0x0, 0xa, 0xfd, 0xde,
    0xf6, 0x0, 0x0, 0x2, 0x80, 0x10, 0x0, 0x0,
    0x2b, 0xf8, 0x0, 0x0, 0xef, 0xff, 0xff, 0xfd,
    0x70, 0xff, 0xff, 0xff, 0xff, 0xf7, 0x68, 0x9f,
    0xfe, 0x8d, 0xfd, 0x0, 0x3, 0x51, 0x4, 0xb9,
    0x0, 0x0, 0x0, 0x0, 0x60, 0x0, 0x0, 0x0,
    0x0, 0xc4, 0x0, 0x0, 0x0, 0x0, 0x0,

    /* U+E50E "" */
    0x0, 0x0, 0x19, 0x60, 0x0, 0x0, 0x0, 0x3,
    0xef, 0xfa, 0x10, 0x0, 0x0, 0x8f, 0xff, 0xff,
    0xe3, 0x0, 0x2, 0xff, 0xff, 0xff, 0xfc, 0x0,
    0x0, 0x4f, 0xff, 0xff, 0xe0, 0x0, 0x0, 0x2c,
    0xb5, 0x7d, 0x90, 0x0, 0x19, 0xa4, 0x4a, 0x83,
    0x6b, 0x60, 0xbd, 0xbf, 0xfb, 0xdf, 0xea, 0xf5,
    0x6, 0x71, 0x17, 0x51, 0x38, 0x30, 0xaf, 0xef,
    0xfd, 0xff, 0xfd, 0xf4, 0x11, 0x3, 0x20, 0x3,
    0x10, 0x20,

    /* U+F06D "" */
    0x0, 0x3b, 0x10, 0x0, 0x0, 0x2e, 0xfc, 0x8c,
    0x0, 0xc, 0xff, 0xff, 0xf9, 0x6, 0xff, 0xff,
    0xff, 0xf2, 0xcf, 0xe4, 0xff, 0xff, 0x8f, 0xf6,
    0x4, 0x9a, 0xfb, 0xdf, 0x40, 0x0, 0x7f, 0x98,
    0xfb, 0x0, 0x1d, 0xf4, 0xd, 0xfe, 0xcf, 0xfa,
    0x0, 0x1a, 0xff, 0xf8, 0x0, 0x0, 0x0, 0x10,
    0x0, 0x0,

    /* U+F2DC "" */
    0x0, 0x0, 0x2b, 0x0, 0x0, 0x0, 0x23, 0xef,
    0xd1, 0x10, 0x8, 0xd5, 0x7f, 0x39, 0xb6, 0xa,
    0xfc, 0x6f, 0x3e, 0xf6, 0xa, 0x9c, 0xff, 0xfa,
    0xa7, 0x6, 0x47, 0xff, 0xe5, 0x54, 0xa, 0xfe,
    0x9f, 0x8f, 0xf6, 0xc, 0xe6, 0x5f, 0x1a, 0xe9,
    0x0, 0x53, 0xff, 0xd3, 0x30, 0x0, 0x0, 0x5f,
    0x30, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 160, .box_w = 10, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 45, .adv_w = 160, .box_w = 10, .box_h = 11, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 100, .adv_w = 180, .box_w = 12, .box_h = 11, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 166, .adv_w = 140, .box_w = 9, .box_h = 11, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 216, .adv_w = 140, .box_w = 10, .box_h = 11, .ofs_x = -1, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x1, 0x509, 0x1068, 0x12d7
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 57349, .range_length = 4824, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 5, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};

extern const lv_font_t lv_font_montserrat_10;


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t moses_icons_10 = {
#else
lv_font_t moses_icons_10 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 11,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = &lv_font_montserrat_10,
#endif
    .user_data = NULL,
};



#endif /*#if MOSES_ICONS_10*/

