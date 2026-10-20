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
struct button_map {
    int button, x, y, radius;
    char *description;
};

extern struct button_map bm[];

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

/* Simulated key that cycles the device colour, see bm[] */
#define SIM_SKIN_COLOUR_KEY SDLK_F2
#endif

/* for the sim, these function is implemented in uisimulator/buttonmap/ *.c */
int key_to_button(int keyboard_button);
#ifdef HAVE_TOUCHSCREEN
int key_to_touch(int keyboard_button, unsigned int mouse_coords);
#endif

#endif /* __BUTTONMAP_H__ */
