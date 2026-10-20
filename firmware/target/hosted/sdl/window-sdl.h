/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
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

#ifndef __WINDOWSDL_H__
#define __WINDOWSDL_H__

#include "SDL.h"

extern SDL_Texture *gui_texture; /* Window content, including background */
extern SDL_Surface *sim_lcd_surface; /* LCD content */

extern SDL_mutex *window_mutex;  /* prevent concurrent drawing from event thread &
                                    main thread on MS Windows */

/* Renders GUI texture. Sets up new texture, if necessary */
void sdl_window_render(void);

/* Updates size, aspect ratio, and re-renders window content */
bool sdl_window_adjust(void);

/* Needs to be called when window size, scale quality, or background should change */
void sdl_window_adjustment_needed(bool destroy_texture);

/* Creates window, renderer, and LCD surface when app launches */
void sdl_window_setup(void);

/* Changes the colour of the device body and centre button (simulators with
   HAVE_SIM_TOUCHWHEEL): the next preset, or on Android a colour picker.
   True if the window needs redrawing. */
bool sdl_window_change_skin_colour(void);

#ifdef __ANDROID__
/* Called from the event thread when the app goes to or returns from the
   background, which replaces the window's surface */
void sdl_window_set_visible(bool visible);

/* Only the event thread renders; other threads request it */
bool sdl_window_is_render_thread(void);
void sdl_window_request_render(void);

/* Handles render requests on the event thread; true if it was one */
bool sdl_window_handle_event(SDL_Event *event);
#endif

#endif /* #ifndef __WINDOWSDL_H__ */
