/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2010 by Fred Bauer
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

#ifndef __BUTTONMAP_H__
#define __BUTTONMAP_H__
/* Button maps: simulated key, x, y, radius, name */
/* Run sim with --mapping to get coordinates      */
/* or --debugbuttons to check                     */
/* The First matching button is returned          */
/* A radius of BM_RECT(w, h) makes the button a w x h rectangle   */
/* centred on x, y instead                                         */
struct button_map {
    int button, x, y, radius;
    char *description;
};

#define BM_RECT(w, h)   (-((w) << 12 | (h)))
#define BM_RECT_W(b)    (-(b)->radius >> 12)
#define BM_RECT_H(b)    (-(b)->radius & 0xfff)

extern struct button_map bm[];

/* Whether x, y is on the button */
static inline int bm_contains(const struct button_map *b, int x, int y)
{
    if (b->radius < 0)
        return 2 * (x - b->x) >= -BM_RECT_W(b) && 2 * (x - b->x) < BM_RECT_W(b) &&
               2 * (y - b->y) >= -BM_RECT_H(b) && 2 * (y - b->y) < BM_RECT_H(b);
    return (x - b->x) * (x - b->x) + (y - b->y) * (y - b->y) <
           b->radius * b->radius;
}

int xy2button( int x, int y);

#if defined(SIMULATOR) && defined(HAVE_SCROLLWHEEL) && \
    (CONFIG_KEYPAD == IPOD_4G_PAD)
#define HAVE_SIM_TOUCHWHEEL
/* Clickwheel ring on the background image: drag along it to scroll,
 * tap or hold to click the button under the finger. An outer_radius
 * of 0 disables it. */
struct touchwheel_map {
    int x, y, inner_radius, outer_radius;
    int keys[4]; /* simulated keys clicked at top, right, bottom, left */
};

extern const struct touchwheel_map tw;
#endif

#if defined(HAVE_SIM_TOUCHWHEEL) || defined(HAVE_SIM_ANDROID)
/* The device's colour can be changed: through presets on a clickwheel
   iPod's bitmap, or with a colour picker and an SVG device image on
   Android (window-sdl.c) */
#define HAVE_SIM_SKIN_COLOUR
/* Simulated key that changes the device colour, see bm[] */
#define SIM_SKIN_COLOUR_KEY SDLK_F2
#endif

/* for the sim, these function is implemented in uisimulator/buttonmap/ *.c */
int key_to_button(int keyboard_button);
#ifdef HAVE_TOUCHSCREEN
int key_to_touch(int keyboard_button, unsigned int mouse_coords);
#endif

#endif /* __BUTTONMAP_H__ */
