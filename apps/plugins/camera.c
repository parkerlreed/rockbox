/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* The phone's camera for the simulator on Android, at the screen's own
 * size and colours: four dithered greys on the iPod mini, like a Game Boy
 * Camera, and colour (or the Game Boy's greens) on colour screens. Photos
 * are saved as PNGs, scaled up in whole pixels to about 480 high. */

#include "plugin.h"
#include "lib/helper.h"

#if (CONFIG_KEYPAD == IPOD_4G_PAD)
#define CAMERA_SHOOT    BUTTON_SELECT
#define CAMERA_FLIP     BUTTON_LEFT
#define CAMERA_FLIP2    BUTTON_RIGHT
#define CAMERA_BRIGHTER BUTTON_SCROLL_FWD
#define CAMERA_DARKER   BUTTON_SCROLL_BACK
#define CAMERA_STYLE    BUTTON_PLAY
#define CAMERA_QUIT     BUTTON_MENU
#define CAMERA_QUIT2    BUTTON_MENU
#elif (CONFIG_KEYPAD == SANSA_CLIP_PAD)
#define CAMERA_SHOOT    BUTTON_SELECT
#define CAMERA_FLIP     BUTTON_LEFT
#define CAMERA_FLIP2    BUTTON_RIGHT
#define CAMERA_BRIGHTER BUTTON_UP
#define CAMERA_DARKER   BUTTON_DOWN
#define CAMERA_STYLE    BUTTON_VOL_UP
#define CAMERA_STYLE2   BUTTON_VOL_DOWN
#define CAMERA_QUIT     BUTTON_HOME
#define CAMERA_QUIT2    BUTTON_POWER
#elif (CONFIG_KEYPAD == GIGABEAT_PAD)
#define CAMERA_SHOOT    BUTTON_SELECT
#define CAMERA_FLIP     BUTTON_LEFT
#define CAMERA_FLIP2    BUTTON_RIGHT
#define CAMERA_BRIGHTER BUTTON_UP
#define CAMERA_DARKER   BUTTON_DOWN
#define CAMERA_STYLE    BUTTON_A
#define CAMERA_QUIT     BUTTON_MENU
#define CAMERA_QUIT2    BUTTON_POWER
#else
#error No keymap defined!
#endif

#ifndef CAMERA_STYLE2
#define CAMERA_STYLE2   CAMERA_STYLE
#endif

#define PHOTO_DIR   "/DCIM/Rockbox"
/* 552x440 on the mini, 480x480 on the Clip Zip, 640x480 on the iPod Video */
#define PHOTO_SCALE MAX(1, 480 / MIN(LCD_WIDTH, LCD_HEIGHT))
#define PHOTO_WIDTH  (LCD_WIDTH * PHOTO_SCALE)
#define PHOTO_HEIGHT (LCD_HEIGHT * PHOTO_SCALE)
#define PIXELS (LCD_WIDTH * LCD_HEIGHT)

static unsigned char grey[PIXELS];  /* brightness */
static unsigned char shade[PIXELS]; /* 0 (dark) to 3 */

#ifdef HAVE_LCD_COLOR
static unsigned char rgb[PIXELS * 3]; /* from the camera */
static fb_data picture[PIXELS];       /* on the screen */

/* the original Game Boy's, darkest first */
static const unsigned char dmg_palette[4][3] = {
    { 0x0f, 0x38, 0x0f }, { 0x30, 0x62, 0x30 },
    { 0x8b, 0xac, 0x0f }, { 0x9b, 0xbc, 0x0f },
};

static bool game_boy; /* or colour */
#else
static const unsigned lcd_shades[4] = {
    LCD_BLACK, LCD_DARKGRAY, LCD_LIGHTGRAY, LCD_WHITE
};

static bool dither = true;
#endif

static const unsigned char bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static bool front;
static int exposure; /* brightness, -64 to 64 */
static int photo_number;

static inline int clamp255(int v)
{
    return MIN(255, MAX(0, v));
}

/* Stretch the 2nd to 98th percentile to the full range, like the Game Boy
   Camera's auto exposure, then dither to four shades */
static void make_shades(bool dithered)
{
    static unsigned hist[256];
    int lo = 0, hi = 255, sum = 0;

    rb->memset(hist, 0, sizeof hist);
    for (int i = 0; i < PIXELS; i++)
        hist[grey[i]]++;
    while (lo < 255 && (sum += hist[lo]) < PIXELS / 50)
        lo++;
    for (sum = 0; hi > 0 && (sum += hist[hi]) < PIXELS / 50; hi--)
        ;
    if (hi - lo < 16)
    {
        lo = MAX(0, lo - 8);
        hi = lo + 16;
    }

    for (int y = 0; y < LCD_HEIGHT; y++)
    {
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            int v = (grey[y * LCD_WIDTH + x] - lo) * 255 / (hi - lo)
                    + exposure;
            v = clamp255(v) * 3;
            int level = v / 255, frac = v % 255;
            int threshold = dithered ? (bayer[y & 3][x & 3] * 2 + 1) * 255 / 32
                                     : 127;
            if (frac > threshold)
                level++;
            shade[y * LCD_WIDTH + x] = level;
        }
    }
}

static bool get_frame(void)
{
#ifdef HAVE_LCD_COLOR
    return rb->sim_camera_frame(rgb, LCD_WIDTH, LCD_HEIGHT, true);
#else
    return rb->sim_camera_frame(grey, LCD_WIDTH, LCD_HEIGHT, false);
#endif
}

static void draw_frame(void)
{
#ifdef HAVE_LCD_COLOR
    const unsigned char *p = rgb;
    if (game_boy)
    {
        for (int i = 0; i < PIXELS; i++, p += 3)
            grey[i] = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
        make_shades(true);
        for (int i = 0; i < PIXELS; i++)
        {
            const unsigned char *c = dmg_palette[shade[i]];
            picture[i] = FB_RGBPACK(c[0], c[1], c[2]);
        }
    }
    else
    {
        for (int i = 0; i < PIXELS; i++, p += 3)
            picture[i] = FB_RGBPACK(clamp255(p[0] + exposure),
                                    clamp255(p[1] + exposure),
                                    clamp255(p[2] + exposure));
    }
    rb->lcd_bitmap(picture, 0, 0, LCD_WIDTH, LCD_HEIGHT);
#else
    make_shades(dither);
    for (int y = 0; y < LCD_HEIGHT; y++)
    {
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            rb->lcd_set_foreground(lcd_shades[shade[y * LCD_WIDTH + x]]);
            rb->lcd_drawpixel(x, y);
        }
    }
#endif
}

/* White with a black shadow, to show on any picture */
static void draw_text(int x, int y, const char *text)
{
    rb->lcd_set_drawmode(DRMODE_FG);
    rb->lcd_set_foreground(LCD_BLACK);
    rb->lcd_putsxy(x + 1, y + 1, text);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_putsxy(x, y, text);
    rb->lcd_set_drawmode(DRMODE_SOLID);
}

static void draw_message(const char *line1, const char *line2)
{
    int w, h;
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_clear_display();
    rb->lcd_getstringsize(line1, &w, &h);
    draw_text((LCD_WIDTH - w) / 2, LCD_HEIGHT / 2 - h, line1);
    rb->lcd_getstringsize(line2, &w, NULL);
    draw_text((LCD_WIDTH - w) / 2, LCD_HEIGHT / 2, line2);
}

/* Writing a PNG */
static int png_fd;
static bool png_ok;
static uint32_t png_crc;

static void png_put(const void *data, size_t len)
{
    if (png_ok)
        png_ok = rb->write(png_fd, data, len) == (ssize_t)len;
    png_crc = rb->crc_32r(data, len, png_crc);
}

static void png_put32(uint32_t v)
{
    unsigned char b[4] = { v >> 24, v >> 16, v >> 8, v };
    png_put(b, 4);
}

static void png_chunk_start(const char *type, uint32_t len)
{
    png_put32(len);
    png_crc = 0xffffffff;
    png_put(type, 4);
}

static void png_chunk_end(void)
{
    png_put32(~png_crc);
}

/* The compressed picture goes out in IDAT chunks of up to this much */
static unsigned char idat[8192];
static int idat_len;

static void idat_flush(void)
{
    if (!idat_len)
        return;
    png_chunk_start("IDAT", idat_len);
    png_put(idat, idat_len);
    png_chunk_end();
    idat_len = 0;
}

/* Deflate with the fixed Huffman codes, matching only the pixel to the
   left and the one above: enough for pictures scaled up in whole pixels */
static uint32_t z_bitbuf;
static int z_bitcount;
static uint32_t adler_a, adler_b;

static void z_byte(unsigned char b)
{
    idat[idat_len++] = b;
    if (idat_len == (int)sizeof idat)
        idat_flush();
}

static void z_bits(uint32_t value, int count)
{
    z_bitbuf |= value << z_bitcount;
    z_bitcount += count;
    while (z_bitcount >= 8)
    {
        z_byte(z_bitbuf);
        z_bitbuf >>= 8;
        z_bitcount -= 8;
    }
}

/* Huffman codes go most significant bit first */
static void z_code(uint32_t code, int count)
{
    uint32_t reversed = 0;
    for (int i = 0; i < count; i++, code >>= 1)
        reversed = reversed << 1 | (code & 1);
    z_bits(reversed, count);
}

static void z_symbol(int s)
{
    if (s < 144)
        z_code(0x30 + s, 8);
    else if (s < 256)
        z_code(0x190 + s - 144, 9);
    else if (s < 280)
        z_code(s - 256, 7);
    else
        z_code(0xc0 + s - 280, 8);
}

static void z_match(int len, int dist)
{
    static const unsigned short len_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const unsigned char len_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const unsigned short dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const unsigned char dist_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    int i = 28, j = 29;
    while (len_base[i] > len)
        i--;
    while (dist_base[j] > dist)
        j--;
    z_symbol(257 + i);
    z_bits(len - len_base[i], len_extra[i]);
    z_code(j, 5);
    z_bits(dist - dist_base[j], dist_extra[j]);
}

/* The row before and this one, each a filter byte and the pixels */
#ifdef HAVE_LCD_COLOR
#define PNG_BPP 3   /* 8-bit RGB */
#define PNG_ROW (1 + PHOTO_WIDTH * 3)
#else
#define PNG_BPP 1   /* 2-bit palette, matched by bytes */
#define PNG_ROW (1 + (PHOTO_WIDTH * 2 + 7) / 8)
#endif
static unsigned char rows[2 * PNG_ROW];

static void z_row(bool first)
{
    const int lowest = first ? PNG_ROW : 0;
    for (int i = PNG_ROW; i < 2 * PNG_ROW; i++)
    {
        adler_a = (adler_a + rows[i]) % 65521;
        adler_b = (adler_b + adler_a) % 65521;
    }
    for (int p = PNG_ROW; p < 2 * PNG_ROW; )
    {
        static const int dists[2] = { PNG_BPP, PNG_ROW };
        int best = 0, best_dist = 0;
        int max = MIN(258, 2 * PNG_ROW - p);
        for (int k = 0; k < 2; k++)
        {
            int d = dists[k], n = 0;
            if (p - d < lowest)
                continue;
            while (n < max && rows[p + n] == rows[p - d + n])
                n++;
            if (n > best)
            {
                best = n;
                best_dist = d;
            }
        }
        if (best >= 3)
        {
            z_match(best, best_dist);
            p += best;
        }
        else
            z_symbol(rows[p++]);
    }
}

static void make_row(unsigned char *row, int y)
{
    int sy = y / PHOTO_SCALE;
    row[0] = 0; /* no filter */
#ifdef HAVE_LCD_COLOR
    const fb_data *src = &picture[sy * LCD_WIDTH];
    unsigned char *dst = &row[1];
    for (int x = 0; x < PHOTO_WIDTH; x++)
    {
        unsigned c = FB_UNPACK_SCALAR_LCD(src[x / PHOTO_SCALE]);
        *dst++ = RGB_UNPACK_RED(c);
        *dst++ = RGB_UNPACK_GREEN(c);
        *dst++ = RGB_UNPACK_BLUE(c);
    }
#else
    const unsigned char *src = &shade[sy * LCD_WIDTH];
    rb->memset(&row[1], 0, PNG_ROW - 1);
    for (int x = 0; x < PHOTO_WIDTH; x++)
        row[1 + x / 4] |= src[x / PHOTO_SCALE] << (6 - 2 * (x & 3));
#endif
}

#ifndef HAVE_LCD_COLOR
/* The simulator's LCD palette with the backlight on (lcd-bitmap.c): shades
   0, 43, 85 and 128 of 129 from LCD_BL_DARKCOLOR to LCD_BL_BRIGHTCOLOR */
static void png_palette(void)
{
    static const int index[4] = { 0, 43, 85, 128 };
    png_chunk_start("PLTE", 4 * 3);
    for (int i = 0; i < 4; i++)
    {
        for (int shift = 16; shift >= 0; shift -= 8)
        {
            int dark = (LCD_BL_DARKCOLOR >> shift) & 0xff;
            int bright = (LCD_BL_BRIGHTCOLOR >> shift) & 0xff;
            unsigned char c = dark + (bright - dark) * index[i] / 128;
            png_put(&c, 1);
        }
    }
    png_chunk_end();
}
#endif

static bool save_photo(char *name, size_t size)
{
    static const unsigned char signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };

    rb->mkdir("/DCIM");
    rb->mkdir(PHOTO_DIR);
    do
        rb->snprintf(name, size, PHOTO_DIR "/IMG_%04d.png", ++photo_number);
    while (rb->file_exists(name) && photo_number < 9999);

    png_fd = rb->open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (png_fd < 0)
        return false;
    png_ok = true;

    png_put(signature, sizeof signature);
    png_chunk_start("IHDR", 13);
    png_put32(PHOTO_WIDTH);
    png_put32(PHOTO_HEIGHT);
#ifdef HAVE_LCD_COLOR
    png_put("\x08\x02\x00\x00\x00", 5); /* 8-bit RGB */
    png_chunk_end();
#else
    png_put("\x02\x03\x00\x00\x00", 5); /* 2-bit palette */
    png_chunk_end();
    png_palette();
#endif

    idat_len = 0;
    z_bitbuf = 0;
    z_bitcount = 0;
    adler_a = 1;
    adler_b = 0;
    z_byte(0x78); /* zlib, deflate with a 32K window */
    z_byte(0x01);
    z_bits(1, 1); /* the last block */
    z_bits(1, 2); /* fixed Huffman codes */
    for (int y = 0; y < PHOTO_HEIGHT; y++)
    {
        rb->memcpy(rows, &rows[PNG_ROW], PNG_ROW);
        make_row(&rows[PNG_ROW], y);
        z_row(y == 0);
    }
    z_symbol(256); /* end of block */
    if (z_bitcount)
        z_bits(0, 8 - z_bitcount);
    uint32_t adler = adler_b << 16 | adler_a;
    for (int shift = 24; shift >= 0; shift -= 8)
        z_byte(adler >> shift);
    idat_flush();

    png_chunk_start("IEND", 0);
    png_chunk_end();

    rb->close(png_fd);
    if (!png_ok)
        rb->remove(name);
    return png_ok;
}

static void take_photo(void)
{
    char name[MAX_PATH];

    /* the shutter: a white flash */
    rb->lcd_set_background(LCD_WHITE);
    rb->lcd_clear_display();
    rb->lcd_update();
    rb->sleep(HZ / 20);
    draw_frame();
    rb->lcd_update();

    if (save_photo(name, sizeof name))
    {
        rb->sim_camera_saved(name);
        rb->splashf(HZ, "Saved %s", rb->strrchr(name, '/') + 1);
    }
    else
        rb->splash(HZ * 2, "Saving failed");
}

static void toggle_style(void)
{
#ifdef HAVE_LCD_COLOR
    game_boy = !game_boy;
    rb->splash(HZ / 2, game_boy ? "Game Boy" : "Colour");
#else
    dither = !dither;
#endif
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    long hint_until = *rb->current_tick + HZ * 3;
    long exposure_until = 0;
    enum plugin_status status = PLUGIN_OK;

    backlight_ignore_timeout();
    rb->lcd_set_drawmode(DRMODE_SOLID);
    rb->sim_camera_open(front);

    while (true)
    {
        int state = rb->sim_camera_state();
        bool have_frame = state == SIM_CAMERA_ON && get_frame();

        if (have_frame)
        {
            draw_frame();
            if (TIME_BEFORE(*rb->current_tick, hint_until))
                draw_text(2, 2, "Select: snap");
            if (TIME_BEFORE(*rb->current_tick, exposure_until))
            {
                char text[16];
                int h;
                rb->snprintf(text, sizeof text, "Bright %+d", exposure / 8);
                rb->lcd_getstringsize(text, NULL, &h);
                draw_text(2, LCD_HEIGHT - h - 2, text);
            }
        }
        else if (state == SIM_CAMERA_DENIED)
            draw_message("No camera", "permission");
        else if (state == SIM_CAMERA_FAILED)
            draw_message("No camera,", "Select retries");
        else
            draw_message("Starting", "the camera");
        rb->lcd_update();

        int button = rb->button_get_w_tmo(HZ / 30);
        if (button == BUTTON_NONE)
            continue;
        if ((button & (BUTTON_REL | BUTTON_REPEAT)) &&
            (button & ~BUTTON_REPEAT) != CAMERA_BRIGHTER &&
            (button & ~BUTTON_REPEAT) != CAMERA_DARKER)
            continue;

        switch (button & ~BUTTON_REPEAT)
        {
        case CAMERA_SHOOT:
            if (have_frame)
                take_photo();
            else if (state == SIM_CAMERA_FAILED || state == SIM_CAMERA_DENIED)
                rb->sim_camera_open(front);
            break;
        case CAMERA_FLIP:
        case CAMERA_FLIP2:
            front = !front;
            rb->sim_camera_open(front);
            rb->splash(HZ / 2, front ? "Front camera" : "Rear camera");
            break;
        case CAMERA_BRIGHTER:
            exposure = MIN(64, exposure + 8);
            exposure_until = *rb->current_tick + HZ;
            break;
        case CAMERA_DARKER:
            exposure = MAX(-64, exposure - 8);
            exposure_until = *rb->current_tick + HZ;
            break;
        case CAMERA_STYLE:
#if CAMERA_STYLE2 != CAMERA_STYLE
        case CAMERA_STYLE2:
#endif
            toggle_style();
            break;
        case CAMERA_QUIT:
#if CAMERA_QUIT2 != CAMERA_QUIT
        case CAMERA_QUIT2:
#endif
            goto done;
        default:
            if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
            {
                status = PLUGIN_USB_CONNECTED;
                goto done;
            }
            break;
        }
    }
done:
    rb->sim_camera_close();
    backlight_use_settings();
    return status;
}
