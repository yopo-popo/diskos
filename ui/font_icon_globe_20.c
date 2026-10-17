/* SPDX-License-Identifier: OFL-1.1 */
/* Glyph data converted from Font Awesome Free (SIL OFL 1.1); licence text: licenses/OFL-1.1-FontAwesome.txt */
/* One glyph, the globe (U+F0AC): the Library jump grid's language switch */
/*******************************************************************************
 * Size: 20 px
 * Bpp: 4
 * Opts: --font fontawesome-webfont.ttf (Font Awesome 4.7.0) --size 20 --bpp 4 --format lvgl --range 0xF0AC --no-compress --force-fast-kern-format -o font_icon_globe_20.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef FONT_ICON_GLOBE_20
#define FONT_ICON_GLOBE_20 1
#endif

#if FONT_ICON_GLOBE_20

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F0AC "" */
    0x0, 0x0, 0x2, 0x7a, 0xba, 0x73, 0x0, 0x0,
    0x0, 0x0, 0x1, 0xaf, 0xff, 0xff, 0xff, 0xb2,
    0x0, 0x0, 0x0, 0x3e, 0xe9, 0x76, 0x7b, 0xff,
    0xff, 0x40, 0x0, 0x1, 0xec, 0x10, 0x0, 0xbf,
    0xbe, 0xff, 0xf3, 0x0, 0xc, 0xe1, 0x0, 0x0,
    0x9f, 0x42, 0x7f, 0xfd, 0x0, 0x4f, 0xd1, 0x0,
    0x0, 0x3, 0x10, 0x3b, 0xff, 0x70, 0xaf, 0xf6,
    0x0, 0x0, 0x0, 0x2, 0xef, 0xff, 0xc0, 0xef,
    0xf5, 0x0, 0x0, 0x0, 0x3e, 0xff, 0xff, 0xf0,
    0xff, 0xfd, 0x10, 0x0, 0x0, 0xbf, 0xff, 0xff,
    0xf1, 0xff, 0xff, 0xc1, 0x4, 0x97, 0xff, 0xff,
    0xff, 0xf1, 0xdf, 0xff, 0xfc, 0xb, 0xff, 0xff,
    0xff, 0xff, 0xf0, 0x9f, 0xff, 0xff, 0x85, 0x6f,
    0xff, 0xff, 0xff, 0xb0, 0x3f, 0xff, 0xff, 0xff,
    0xa9, 0xed, 0xff, 0xff, 0x40, 0x9, 0xff, 0xff,
    0xff, 0xfd, 0x40, 0x1c, 0xfb, 0x0, 0x0, 0xcf,
    0xff, 0xff, 0xff, 0x20, 0x3e, 0xe1, 0x0, 0x0,
    0xb, 0xff, 0xff, 0xfe, 0x4a, 0xfc, 0x10, 0x0,
    0x0, 0x0, 0x6d, 0xff, 0xff, 0xfe, 0x70, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x25, 0x65, 0x30, 0x0,
    0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 274, .box_w = 18, .box_h = 18, .ofs_x = 0, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61612, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
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



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t font_icon_globe_20 = {
#else
lv_font_t font_icon_globe_20 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 18,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if FONT_ICON_GLOBE_20*/

