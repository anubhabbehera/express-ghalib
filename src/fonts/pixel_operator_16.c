/*******************************************************************************
 * Size: 16 px
 * Bpp: 1
 * Opts: --font tools/fonts/PixelOperator.ttf --size 16 --bpp 1 --no-compress --range 0x20-0x7E,0xA0-0xFF --format lvgl --lv-font-name pixel_operator_16 -o src/fonts/pixel_operator_16.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef PIXEL_OPERATOR_16
#define PIXEL_OPERATOR_16 1
#endif

#if PIXEL_OPERATOR_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xfe, 0x80,

    /* U+0022 "\"" */
    0xb6, 0x80,

    /* U+0023 "#" */
    0x49, 0x2f, 0xd2, 0x49, 0x2f, 0xd2, 0x48,

    /* U+0024 "$" */
    0x21, 0x1d, 0x5a, 0x51, 0xc5, 0x2d, 0x5c, 0x42,
    0x0,

    /* U+0025 "%" */
    0x41, 0x42, 0x92, 0x41, 0x4, 0x92, 0x85, 0x4,

    /* U+0026 "&" */
    0x74, 0x61, 0x7, 0x46, 0x31, 0x78,

    /* U+0027 "'" */
    0xe0,

    /* U+0028 "(" */
    0x2a, 0x49, 0x22, 0x20,

    /* U+0029 ")" */
    0x88, 0x92, 0x4a, 0x80,

    /* U+002A "*" */
    0x25, 0x5d, 0x52, 0x0,

    /* U+002B "+" */
    0x21, 0x3e, 0x42, 0x0,

    /* U+002C "," */
    0x58,

    /* U+002D "-" */
    0xf0,

    /* U+002E "." */
    0x80,

    /* U+002F "/" */
    0x24, 0xa4, 0xa4, 0x80,

    /* U+0030 "0" */
    0x74, 0x63, 0x3a, 0xe6, 0x31, 0x70,

    /* U+0031 "1" */
    0x2e, 0x92, 0x49, 0x20,

    /* U+0032 "2" */
    0x74, 0x42, 0x22, 0x22, 0x10, 0xf8,

    /* U+0033 "3" */
    0x74, 0x42, 0x13, 0x4, 0x31, 0x70,

    /* U+0034 "4" */
    0x8, 0xca, 0x98, 0xfc, 0x21, 0x8,

    /* U+0035 "5" */
    0xfc, 0x21, 0xe0, 0x84, 0x31, 0x70,

    /* U+0036 "6" */
    0x74, 0x61, 0xf, 0x46, 0x31, 0x70,

    /* U+0037 "7" */
    0xf8, 0x42, 0x22, 0x22, 0x10, 0x80,

    /* U+0038 "8" */
    0x74, 0x63, 0x17, 0x46, 0x31, 0x70,

    /* U+0039 "9" */
    0x74, 0x63, 0x17, 0x84, 0x31, 0x70,

    /* U+003A ":" */
    0x82,

    /* U+003B ";" */
    0x40, 0x5, 0x80,

    /* U+003C "<" */
    0x2a, 0x22,

    /* U+003D "=" */
    0xf0, 0xf0,

    /* U+003E ">" */
    0x88, 0xa8,

    /* U+003F "?" */
    0x74, 0x42, 0x22, 0x10, 0x80, 0x20,

    /* U+0040 "@" */
    0x7d, 0x6, 0x6d, 0x5a, 0xb5, 0x67, 0x40, 0x7c,

    /* U+0041 "A" */
    0x74, 0x63, 0x18, 0xfe, 0x31, 0x88,

    /* U+0042 "B" */
    0xf4, 0x63, 0x1f, 0x46, 0x31, 0xf0,

    /* U+0043 "C" */
    0x74, 0x61, 0x8, 0x42, 0x11, 0x70,

    /* U+0044 "D" */
    0xf4, 0x63, 0x18, 0xc6, 0x31, 0xf0,

    /* U+0045 "E" */
    0xfc, 0x21, 0xe, 0x42, 0x10, 0xf8,

    /* U+0046 "F" */
    0xfc, 0x21, 0xe, 0x42, 0x10, 0x80,

    /* U+0047 "G" */
    0x74, 0x61, 0x9, 0xc6, 0x31, 0x78,

    /* U+0048 "H" */
    0x8c, 0x63, 0x1f, 0xc6, 0x31, 0x88,

    /* U+0049 "I" */
    0xff, 0x80,

    /* U+004A "J" */
    0x8, 0x42, 0x10, 0x84, 0x31, 0x70,

    /* U+004B "K" */
    0x8c, 0x65, 0x4c, 0x52, 0x51, 0x88,

    /* U+004C "L" */
    0x84, 0x21, 0x8, 0x42, 0x10, 0xf8,

    /* U+004D "M" */
    0x83, 0x7, 0x1d, 0x59, 0x30, 0x60, 0xc1, 0x82,

    /* U+004E "N" */
    0x8c, 0x63, 0x9a, 0xce, 0x31, 0x88,

    /* U+004F "O" */
    0x74, 0x63, 0x18, 0xc6, 0x31, 0x70,

    /* U+0050 "P" */
    0xf4, 0x63, 0x1f, 0x42, 0x10, 0x80,

    /* U+0051 "Q" */
    0x74, 0x63, 0x18, 0xc6, 0xb2, 0x68,

    /* U+0052 "R" */
    0xf4, 0x63, 0x1f, 0x52, 0x51, 0x88,

    /* U+0053 "S" */
    0x74, 0x61, 0x7, 0x4, 0x31, 0x70,

    /* U+0054 "T" */
    0xf9, 0x8, 0x42, 0x10, 0x84, 0x20,

    /* U+0055 "U" */
    0x8c, 0x63, 0x18, 0xc6, 0x31, 0x70,

    /* U+0056 "V" */
    0x8c, 0x63, 0x18, 0xc6, 0x2a, 0x20,

    /* U+0057 "W" */
    0x83, 0x6, 0x4c, 0x99, 0x32, 0x64, 0xc9, 0x6c,

    /* U+0058 "X" */
    0x8c, 0x62, 0xa2, 0x2a, 0x31, 0x88,

    /* U+0059 "Y" */
    0x8c, 0x62, 0xa2, 0x10, 0x84, 0x20,

    /* U+005A "Z" */
    0xf8, 0x42, 0x22, 0x22, 0x10, 0xf8,

    /* U+005B "[" */
    0xf2, 0x49, 0x24, 0xe0,

    /* U+005C "\\" */
    0x92, 0x24, 0x89, 0x20,

    /* U+005D "]" */
    0xe4, 0x92, 0x49, 0xe0,

    /* U+005E "^" */
    0x22, 0xa2,

    /* U+005F "_" */
    0xf8,

    /* U+0060 "`" */
    0x90,

    /* U+0061 "a" */
    0x74, 0x42, 0xf8, 0xc5, 0xe0,

    /* U+0062 "b" */
    0x84, 0x3d, 0x18, 0xc6, 0x31, 0xf0,

    /* U+0063 "c" */
    0x74, 0x61, 0x8, 0x45, 0xc0,

    /* U+0064 "d" */
    0x8, 0x5f, 0x18, 0xc6, 0x31, 0x78,

    /* U+0065 "e" */
    0x74, 0x63, 0xf8, 0x45, 0xc0,

    /* U+0066 "f" */
    0x34, 0x4f, 0x44, 0x44, 0x40,

    /* U+0067 "g" */
    0x7c, 0x63, 0x18, 0xbc, 0x31, 0x70,

    /* U+0068 "h" */
    0x84, 0x3d, 0x18, 0xc6, 0x31, 0x88,

    /* U+0069 "i" */
    0xbf, 0x80,

    /* U+006A "j" */
    0x8, 0x2, 0x10, 0x84, 0x21, 0xc, 0x5c,

    /* U+006B "k" */
    0x84, 0x23, 0x2a, 0x62, 0x92, 0x88,

    /* U+006C "l" */
    0xaa, 0xaa, 0x40,

    /* U+006D "m" */
    0xed, 0x26, 0x4c, 0x99, 0x30, 0x60, 0x80,

    /* U+006E "n" */
    0xf4, 0x63, 0x18, 0xc6, 0x20,

    /* U+006F "o" */
    0x74, 0x63, 0x18, 0xc5, 0xc0,

    /* U+0070 "p" */
    0xf4, 0x63, 0x18, 0xc7, 0xd0, 0x80,

    /* U+0071 "q" */
    0x7c, 0x63, 0x18, 0xc5, 0xe1, 0x8,

    /* U+0072 "r" */
    0x9d, 0x31, 0x8, 0x42, 0x0,

    /* U+0073 "s" */
    0x74, 0x60, 0xe0, 0xc5, 0xc0,

    /* U+0074 "t" */
    0x4f, 0x44, 0x44, 0x43,

    /* U+0075 "u" */
    0x8c, 0x63, 0x18, 0xc5, 0xc0,

    /* U+0076 "v" */
    0x8c, 0x63, 0x18, 0xa8, 0x80,

    /* U+0077 "w" */
    0x83, 0x6, 0x4c, 0x99, 0x32, 0x5b, 0x0,

    /* U+0078 "x" */
    0x8c, 0x54, 0x45, 0x46, 0x20,

    /* U+0079 "y" */
    0x8c, 0x63, 0x18, 0xbc, 0x31, 0x70,

    /* U+007A "z" */
    0xf8, 0x44, 0x44, 0x43, 0xe0,

    /* U+007B "{" */
    0x34, 0x44, 0x84, 0x44, 0x30,

    /* U+007C "|" */
    0xff, 0x80,

    /* U+007D "}" */
    0xc2, 0x22, 0x12, 0x22, 0xc0,

    /* U+007E "~" */
    0x66, 0x60,

    /* U+00A0 " " */
    0x0,

    /* U+00A1 "¡" */
    0xbf, 0x80,

    /* U+00A2 "¢" */
    0x21, 0x1d, 0x5a, 0x52, 0x95, 0x71, 0x8,

    /* U+00A3 "£" */
    0x39, 0x14, 0x10, 0xf1, 0x4, 0x11, 0xf8,

    /* U+00A5 "¥" */
    0x8c, 0x62, 0xa2, 0x7c, 0x9f, 0x20,

    /* U+00A6 "¦" */
    0xf7, 0x80,

    /* U+00A8 "¨" */
    0xa0,

    /* U+00A9 "©" */
    0x7d, 0x6, 0x6d, 0x1a, 0x34, 0x66, 0xc1, 0x7c,

    /* U+00AB "«" */
    0x25, 0x29, 0x12, 0x24,

    /* U+00AC "¬" */
    0xf8, 0x42,

    /* U+00AE "®" */
    0x7d, 0x6, 0xcd, 0x5a, 0xb6, 0x6a, 0xc1, 0x7c,

    /* U+00B0 "°" */
    0x69, 0x96,

    /* U+00B1 "±" */
    0x21, 0x3e, 0x42, 0x3, 0xe0,

    /* U+00B4 "´" */
    0x60,

    /* U+00B5 "µ" */
    0x8c, 0x63, 0x18, 0xc7, 0xd0, 0x80,

    /* U+00B6 "¶" */
    0x7f, 0xe7, 0xcf, 0x97, 0x22, 0x44, 0x89, 0x12,

    /* U+00B7 "·" */
    0x80,

    /* U+00B8 "¸" */
    0x47, 0x0,

    /* U+00BB "»" */
    0x91, 0x22, 0x52, 0x90,

    /* U+00BF "¿" */
    0x20, 0x8, 0x42, 0x22, 0x11, 0x70,

    /* U+00C0 "À" */
    0x41, 0x0, 0xe8, 0xc6, 0x31, 0xfc, 0x63, 0x10,

    /* U+00C1 "Á" */
    0x11, 0x0, 0xe8, 0xc6, 0x31, 0xfc, 0x63, 0x10,

    /* U+00C2 "Â" */
    0x22, 0x80, 0xe8, 0xc6, 0x31, 0xfc, 0x63, 0x10,

    /* U+00C3 "Ã" */
    0x66, 0x60, 0x1c, 0x8a, 0x28, 0xa2, 0xfa, 0x28,
    0xa2,

    /* U+00C4 "Ä" */
    0x50, 0x1d, 0x18, 0xc6, 0x3f, 0x8c, 0x62,

    /* U+00C5 "Å" */
    0x22, 0x88, 0x7, 0x46, 0x31, 0x8f, 0xe3, 0x18,
    0x80,

    /* U+00C6 "Æ" */
    0x7f, 0xc4, 0x22, 0x11, 0x8, 0xe7, 0xc2, 0x21,
    0x10, 0x8f, 0x80,

    /* U+00C7 "Ç" */
    0x74, 0x61, 0x8, 0x42, 0x11, 0x71, 0x4, 0xc0,

    /* U+00C8 "È" */
    0x41, 0x1, 0xf8, 0x42, 0x1c, 0x84, 0x21, 0xf0,

    /* U+00C9 "É" */
    0x11, 0x1, 0xf8, 0x42, 0x1c, 0x84, 0x21, 0xf0,

    /* U+00CA "Ê" */
    0x22, 0x81, 0xf8, 0x42, 0x1c, 0x84, 0x21, 0xf0,

    /* U+00CB "Ë" */
    0x50, 0x3f, 0x8, 0x43, 0x90, 0x84, 0x3e,

    /* U+00CC "Ì" */
    0x91, 0x55, 0x55,

    /* U+00CD "Í" */
    0x62, 0xaa, 0xaa,

    /* U+00CE "Î" */
    0x54, 0x24, 0x92, 0x49, 0x20,

    /* U+00CF "Ï" */
    0xa1, 0x24, 0x92, 0x49, 0x0,

    /* U+00D0 "Ð" */
    0x79, 0x14, 0x51, 0xf5, 0x14, 0x51, 0x78,

    /* U+00D1 "Ñ" */
    0x66, 0x60, 0x22, 0x8a, 0x2c, 0xaa, 0x9a, 0x28,
    0xa2,

    /* U+00D2 "Ò" */
    0x41, 0x0, 0xe8, 0xc6, 0x31, 0x8c, 0x62, 0xe0,

    /* U+00D3 "Ó" */
    0x11, 0x0, 0xe8, 0xc6, 0x31, 0x8c, 0x62, 0xe0,

    /* U+00D4 "Ô" */
    0x22, 0x80, 0xe8, 0xc6, 0x31, 0x8c, 0x62, 0xe0,

    /* U+00D5 "Õ" */
    0x66, 0x60, 0x1c, 0x8a, 0x28, 0xa2, 0x8a, 0x28,
    0x9c,

    /* U+00D6 "Ö" */
    0x50, 0x1d, 0x18, 0xc6, 0x31, 0x8c, 0x5c,

    /* U+00D7 "×" */
    0x8a, 0x88, 0xa8, 0x80,

    /* U+00D8 "Ø" */
    0x3a, 0x89, 0x12, 0x65, 0x4c, 0x91, 0x22, 0xb8,

    /* U+00D9 "Ù" */
    0x41, 0x1, 0x18, 0xc6, 0x31, 0x8c, 0x62, 0xe0,

    /* U+00DA "Ú" */
    0x11, 0x1, 0x18, 0xc6, 0x31, 0x8c, 0x62, 0xe0,

    /* U+00DB "Û" */
    0x22, 0x81, 0x18, 0xc6, 0x31, 0x8c, 0x62, 0xe0,

    /* U+00DC "Ü" */
    0x50, 0x23, 0x18, 0xc6, 0x31, 0x8c, 0x5c,

    /* U+00DD "Ý" */
    0x11, 0x1, 0x18, 0xc5, 0x44, 0x21, 0x8, 0x40,

    /* U+00DE "Þ" */
    0x84, 0x3d, 0x18, 0xc7, 0xd0, 0x80,

    /* U+00DF "ß" */
    0x74, 0x63, 0x2a, 0x4a, 0x31, 0xb0,

    /* U+00E0 "à" */
    0x41, 0x0, 0xe8, 0x85, 0xf1, 0x8b, 0xc0,

    /* U+00E1 "á" */
    0x11, 0x0, 0xe8, 0x85, 0xf1, 0x8b, 0xc0,

    /* U+00E2 "â" */
    0x22, 0x80, 0xe8, 0x85, 0xf1, 0x8b, 0xc0,

    /* U+00E3 "ã" */
    0x66, 0x60, 0x1c, 0x88, 0x27, 0xa2, 0x89, 0xe0,

    /* U+00E4 "ä" */
    0x50, 0x1d, 0x10, 0xbe, 0x31, 0x78,

    /* U+00E5 "å" */
    0x22, 0x88, 0x7, 0x44, 0x2f, 0x8c, 0x5e,

    /* U+00E6 "æ" */
    0x77, 0x44, 0x42, 0x2f, 0xf8, 0x84, 0x45, 0xdc,

    /* U+00E7 "ç" */
    0x74, 0x61, 0x8, 0x45, 0xc4, 0x13, 0x0,

    /* U+00E8 "è" */
    0x41, 0x0, 0xe8, 0xc7, 0xf0, 0x8b, 0x80,

    /* U+00E9 "é" */
    0x11, 0x0, 0xe8, 0xc7, 0xf0, 0x8b, 0x80,

    /* U+00EA "ê" */
    0x22, 0x80, 0xe8, 0xc7, 0xf0, 0x8b, 0x80,

    /* U+00EB "ë" */
    0x50, 0x1d, 0x18, 0xfe, 0x11, 0x70,

    /* U+00EC "ì" */
    0x91, 0x55, 0x50,

    /* U+00ED "í" */
    0x62, 0xaa, 0xa0,

    /* U+00EE "î" */
    0x54, 0x24, 0x92, 0x48,

    /* U+00EF "ï" */
    0xa1, 0x24, 0x92, 0x40,

    /* U+00F0 "ð" */
    0x8, 0xf0, 0x9e, 0x8a, 0x28, 0xa2, 0x78,

    /* U+00F1 "ñ" */
    0x66, 0x60, 0x3c, 0x8a, 0x28, 0xa2, 0x8a, 0x20,

    /* U+00F2 "ò" */
    0x41, 0x0, 0xe8, 0xc6, 0x31, 0x8b, 0x80,

    /* U+00F3 "ó" */
    0x11, 0x0, 0xe8, 0xc6, 0x31, 0x8b, 0x80,

    /* U+00F4 "ô" */
    0x22, 0x80, 0xe8, 0xc6, 0x31, 0x8b, 0x80,

    /* U+00F5 "õ" */
    0x66, 0x60, 0x1c, 0x8a, 0x28, 0xa2, 0x89, 0xc0,

    /* U+00F6 "ö" */
    0x50, 0x1d, 0x18, 0xc6, 0x31, 0x70,

    /* U+00F7 "÷" */
    0x20, 0x3e, 0x2, 0x0,

    /* U+00F8 "ø" */
    0x3a, 0x89, 0x32, 0xa6, 0x48, 0xae, 0x0,

    /* U+00F9 "ù" */
    0x41, 0x1, 0x18, 0xc6, 0x31, 0x8b, 0x80,

    /* U+00FA "ú" */
    0x11, 0x1, 0x18, 0xc6, 0x31, 0x8b, 0x80,

    /* U+00FB "û" */
    0x22, 0x81, 0x18, 0xc6, 0x31, 0x8b, 0x80,

    /* U+00FC "ü" */
    0x50, 0x23, 0x18, 0xc6, 0x31, 0x70,

    /* U+00FD "ý" */
    0x11, 0x1, 0x18, 0xc6, 0x31, 0x78, 0x62, 0xe0,

    /* U+00FE "þ" */
    0x84, 0x3d, 0x18, 0xc6, 0x31, 0xf4, 0x20,

    /* U+00FF "ÿ" */
    0x50, 0x23, 0x18, 0xc6, 0x2f, 0xc, 0x5c
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 64, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 80, .box_w = 1, .box_h = 9, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3, .adv_w = 112, .box_w = 3, .box_h = 3, .ofs_x = 2, .ofs_y = 6},
    {.bitmap_index = 5, .adv_w = 128, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 12, .adv_w = 112, .box_w = 5, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 21, .adv_w = 144, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 29, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 35, .adv_w = 80, .box_w = 1, .box_h = 3, .ofs_x = 2, .ofs_y = 6},
    {.bitmap_index = 36, .adv_w = 112, .box_w = 3, .box_h = 9, .ofs_x = 3, .ofs_y = 0},
    {.bitmap_index = 40, .adv_w = 112, .box_w = 3, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 44, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 48, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 52, .adv_w = 80, .box_w = 2, .box_h = 3, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 53, .adv_w = 96, .box_w = 4, .box_h = 1, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 54, .adv_w = 80, .box_w = 1, .box_h = 1, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 55, .adv_w = 80, .box_w = 3, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 59, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 65, .adv_w = 112, .box_w = 3, .box_h = 9, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 69, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 75, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 81, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 87, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 93, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 99, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 105, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 111, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 117, .adv_w = 80, .box_w = 1, .box_h = 7, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 118, .adv_w = 80, .box_w = 2, .box_h = 9, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 121, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 123, .adv_w = 96, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 125, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 127, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 133, .adv_w = 144, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 141, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 147, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 153, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 159, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 165, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 171, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 177, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 183, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 189, .adv_w = 80, .box_w = 1, .box_h = 9, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 191, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 197, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 203, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 209, .adv_w = 144, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 217, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 223, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 229, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 235, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 241, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 247, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 253, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 259, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 265, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 271, .adv_w = 144, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 279, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 285, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 291, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 297, .adv_w = 112, .box_w = 3, .box_h = 9, .ofs_x = 3, .ofs_y = 0},
    {.bitmap_index = 301, .adv_w = 80, .box_w = 3, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 305, .adv_w = 112, .box_w = 3, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 309, .adv_w = 112, .box_w = 5, .box_h = 3, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 311, .adv_w = 80, .box_w = 5, .box_h = 1, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 312, .adv_w = 80, .box_w = 2, .box_h = 2, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 313, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 318, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 324, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 329, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 335, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 340, .adv_w = 96, .box_w = 4, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 345, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 351, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 357, .adv_w = 80, .box_w = 1, .box_h = 9, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 359, .adv_w = 112, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 366, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 372, .adv_w = 80, .box_w = 2, .box_h = 9, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 375, .adv_w = 144, .box_w = 7, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 382, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 387, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 392, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 398, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 404, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 409, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 414, .adv_w = 96, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 418, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 423, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 428, .adv_w = 144, .box_w = 7, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 435, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 440, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 446, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 451, .adv_w = 112, .box_w = 4, .box_h = 9, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 456, .adv_w = 80, .box_w = 1, .box_h = 9, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 458, .adv_w = 112, .box_w = 4, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 463, .adv_w = 128, .box_w = 6, .box_h = 2, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 465, .adv_w = 64, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 466, .adv_w = 80, .box_w = 1, .box_h = 9, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 468, .adv_w = 112, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 475, .adv_w = 128, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 482, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 488, .adv_w = 80, .box_w = 1, .box_h = 9, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 490, .adv_w = 112, .box_w = 3, .box_h = 1, .ofs_x = 2, .ofs_y = 8},
    {.bitmap_index = 491, .adv_w = 144, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 499, .adv_w = 128, .box_w = 6, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 503, .adv_w = 112, .box_w = 5, .box_h = 3, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 505, .adv_w = 144, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 513, .adv_w = 96, .box_w = 4, .box_h = 4, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 515, .adv_w = 112, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 520, .adv_w = 80, .box_w = 2, .box_h = 2, .ofs_x = 2, .ofs_y = 7},
    {.bitmap_index = 521, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 527, .adv_w = 144, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 535, .adv_w = 80, .box_w = 1, .box_h = 1, .ofs_x = 2, .ofs_y = 4},
    {.bitmap_index = 536, .adv_w = 80, .box_w = 3, .box_h = 3, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 538, .adv_w = 128, .box_w = 6, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 542, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 548, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 556, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 564, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 572, .adv_w = 112, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 581, .adv_w = 112, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 588, .adv_w = 112, .box_w = 5, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 597, .adv_w = 176, .box_w = 9, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 608, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 616, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 624, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 632, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 640, .adv_w = 112, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 647, .adv_w = 80, .box_w = 2, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 650, .adv_w = 80, .box_w = 2, .box_h = 12, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 653, .adv_w = 80, .box_w = 3, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 658, .adv_w = 80, .box_w = 3, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 663, .adv_w = 112, .box_w = 6, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 670, .adv_w = 112, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 679, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 687, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 695, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 703, .adv_w = 112, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 712, .adv_w = 112, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 719, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 723, .adv_w = 112, .box_w = 7, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 731, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 739, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 747, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 755, .adv_w = 112, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 762, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 770, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 776, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 782, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 789, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 796, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 803, .adv_w = 112, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 811, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 817, .adv_w = 112, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 824, .adv_w = 176, .box_w = 9, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 832, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 839, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 846, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 853, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 860, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 866, .adv_w = 80, .box_w = 2, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 869, .adv_w = 80, .box_w = 2, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 872, .adv_w = 80, .box_w = 3, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 876, .adv_w = 80, .box_w = 3, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 880, .adv_w = 112, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 887, .adv_w = 112, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 895, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 902, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 909, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 916, .adv_w = 112, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 924, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 930, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 934, .adv_w = 112, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 941, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 948, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 955, .adv_w = 112, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 962, .adv_w = 112, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 968, .adv_w = 112, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 976, .adv_w = 112, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 983, .adv_w = 112, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint8_t glyph_id_ofs_list_1[] = {
    0, 1, 2, 3, 0, 4, 5, 0,
    6, 7, 0, 8, 9, 0, 10, 0,
    11, 12, 0, 0, 13, 14, 15, 16,
    17, 0, 0, 18
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 160, .range_length = 28, .glyph_id_start = 96,
        .unicode_list = NULL, .glyph_id_ofs_list = glyph_id_ofs_list_1, .list_length = 28, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL
    },
    {
        .range_start = 191, .range_length = 65, .glyph_id_start = 115,
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
    .cmap_num = 3,
    .bpp = 1,
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
const lv_font_t pixel_operator_16 = {
#else
lv_font_t pixel_operator_16 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -3,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if PIXEL_OPERATOR_16*/

