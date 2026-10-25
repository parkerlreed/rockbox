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

#ifndef __WINDOW3D_H__
#define __WINDOW3D_H__

#include <stdbool.h>
#include "SDL.h"

/* The device as a 3D model drawn with OpenGL ES (HAVE_SIM_3D), from the
   files uisimulator/skins/ipodmini2g-3d.py makes, in dir */

/* Creates the window and its GL context: NULL without the model or GL ES 3,
   and the caller then makes a window of its own */
SDL_Window *sim3d_create_window(const char *dir, const char *title,
                                int w, int h, Uint32 flags);
bool sim3d_active(void);

struct sim3d_look
{
    SDL_Color body;     /* the case's colour, if tinted */
    bool tinted;        /* else the model's own */
    SDL_Color lcd_bg;   /* round the LCD on the display */
};

/* Draws the device with the LCD's content and shows it; true while it
   moves, when it needs drawing again */
bool sim3d_render(SDL_Surface *lcd, const struct sim3d_look *look);

enum sim3d_hit { SIM3D_NONE, SIM3D_FACE, SIM3D_HOLD };

/* What's under a point in the window; on the face, the point in UI
   coordinates (those of the 2D skin, sim-ui-defines.h) */
enum sim3d_hit sim3d_pick(int x, int y, int *ui_x, int *ui_y);

/* The point on the face's plane under a point in the window, also off the
   device, in UI coordinates */
void sim3d_face_point(int x, int y, int *ui_x, int *ui_y);

/* Turns the device by a drag of dx, dy in the window */
void sim3d_turn(int dx, int dy);
/* Zooms in by factor round a point in the window */
void sim3d_zoom(float factor, int x, int y);
/* Moves the device by dx, dy in the window */
void sim3d_pan(int dx, int dy);
/* Turns it back to its starting position, gradually */
void sim3d_reset_view(void);
/* How many mm a drag from x0, y0 to x, y in the window moves along the
   hold switch's way to hold, back from it if negative */
float sim3d_hold_slide(int x0, int y0, int x, int y);
/* How far the switch slides, in mm */
float sim3d_hold_travel(void);
/* Slides the dock cable in when the phone starts charging, out when it
   stops; true once it's in, when Rockbox should hear of the charger */
bool sim3d_cable(bool plugged);
/* Slides the hold switch */
void sim3d_set_hold(bool on);

#endif /* __WINDOW3D_H__ */
