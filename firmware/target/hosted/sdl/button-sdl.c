/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Felix Arends
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

#include <math.h>
#include <stdlib.h>         /* EXIT_SUCCESS */
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include "sim-ui-defines.h"
#ifdef HAVE_REMOTE_LCD
#include "lcd-remote.h"
#endif
#include "config.h"
#include "button.h"
#include "kernel.h"
#include "backlight.h"
#include "system.h"
#include "button-sdl.h"
#if SDL_MAJOR_VERSION > 1
#include "window-sdl.h"
#endif
#include "sim_tasks.h"
#include "buttonmap.h"
#include "debug.h"
#include "powermgmt.h"
#include "storage.h"

#ifdef HAVE_TOUCHSCREEN
#include "touchscreen.h"
static int mouse_coords = 0;
#endif
/* how long until repeat kicks in */
#define REPEAT_START      6

/* the speed repeat starts at */
#define REPEAT_INTERVAL_START   4

/* speed repeat finishes at */
#define REPEAT_INTERVAL_FINISH  2

#ifdef HAVE_TOUCHSCREEN
#define USB_KEY SDLK_c /* SDLK_u is taken by BUTTON_MIDLEFT */
#else
#define USB_KEY SDLK_u
#endif
#define EXT_KEY SDLK_e

#if defined(IRIVER_H100_SERIES) || defined (IRIVER_H300_SERIES)
int _remote_type=REMOTETYPE_H100_LCD;

int remote_type(void)
{
    return _remote_type;
}
#endif

static int btn = 0;    /* Hopefully keeps track of currently pressed keys... */

#ifdef SIMULATOR
#include "strlcat.h"
static bool cursor_isfocus = false;
SDL_Cursor *sdl_focus_cursor = NULL;
SDL_Cursor *sdl_arrow_cursor = NULL;
#endif

int sdl_app_has_input_focus = 1;

#ifdef HAS_BUTTON_HOLD
bool hold_button_state = false;
bool button_hold(void) {
    return hold_button_state;
}
#endif

#ifdef HAS_REMOTE_BUTTON_HOLD
bool remote_hold_button_state = false;
bool remote_button_hold(void) {
    return remote_hold_button_state;
}
#endif
static void button_event(int key, bool pressed);
extern bool debug_wps;
extern bool mapping;

#ifdef HAVE_TOUCHSCREEN
static void touchscreen_event(int x, int y)
{
    if(background) {
        x -= UI_LCD_POSX;
        y -= UI_LCD_POSY;
    }

    if(x >= 0 && y >= 0 && x < SIM_LCD_WIDTH && y < SIM_LCD_HEIGHT) {
        mouse_coords = (x << 16) | y;
        button_event(BUTTON_TOUCHSCREEN, true);
        if (debug_wps)
            printf("Mouse at 1: (%d, %d)\n", x, y);
    }
}
#endif

#if defined(HAVE_SCROLLWHEEL) || ((defined(BUTTON_SCROLL_FWD) && defined(BUTTON_SCROLL_BACK)))
static void scrollwheel_event(int x, int y)
{
    int new_btn = 0;
    if (y > 0)
        new_btn = BUTTON_SCROLL_BACK;
    else if (y < 0)
        new_btn = BUTTON_SCROLL_FWD;
    else
        return;

#ifdef HAVE_BACKLIGHT
    backlight_on();
#endif
#ifdef HAVE_BUTTON_LIGHT
    buttonlight_on();
#endif
    reset_poweroff_timer();
    if (new_btn && !button_queue_full())
        button_queue_post(new_btn, 1<<24);

    (void)x;
}
#endif

#ifdef HAVE_SIM_TOUCHWHEEL
/* Emulates the touch-sensitive clickwheel with the mouse or a touchscreen.
 * Scrolling follows the clickwheel driver in
 * firmware/target/arm/ipod/button-clickwheel.c */
#define TW_CLICKS_PER_ROTATION 96
#define TW_SENSITIVITY          4   /* clicks per scroll step */
#define TW_HOLD_MS            250   /* finger held still this long = click */
#define TW_TAP_MS              50   /* how long a tap keeps the button down */
#define TW_REPEAT_MS          250
#define TW_FAST_OFF_MS        250

enum { TW_TIMER_HOLD, TW_TIMER_TAP_RELEASE };

static Uint32 tw_event_type = (Uint32)-1;
static enum { TW_IDLE, TW_TOUCHED, TW_SCROLLING, TW_PRESSED } tw_state;
static int tw_key;          /* key under the finger on touch down */
static int tw_tap_key;      /* key held down by a tap, until its timer */
static int tw_pos;          /* wheel position at the last scroll step */
static int tw_dir;          /* direction of the last scroll step */
static Uint32 tw_last_ms;   /* time of the last scroll step */
static unsigned tw_velocity;
static unsigned tw_gen;     /* identifies the current touch for timers */

static Uint32 tw_timer_cb(Uint32 interval, void *param)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = tw_event_type;
    ev.user.code = (int)(intptr_t)param;
    /* SDL_RegisterEvents() hands out SDL_USEREVENT, which power_off()
       also pushes to stop the event loop: tell ours apart */
    ev.user.data1 = &tw_event_type;
    SDL_PushEvent(&ev);
    (void)interval;
    return 0;
}

static void tw_start_timer(int kind, Uint32 ms)
{
    SDL_AddTimer(ms, tw_timer_cb, (void *)(intptr_t)((tw_gen << 1) | kind));
}

static int tw_dist2(int x, int y)
{
    return (x - tw.x) * (x - tw.x) + (y - tw.y) * (y - tw.y);
}

static bool tw_on_ring(int x, int y)
{
    int d2 = tw_dist2(x, y);
    return tw.outer_radius && background && !mapping
        && d2 >= tw.inner_radius * tw.inner_radius
        && d2 <= tw.outer_radius * tw.outer_radius;
}

/* 0..95, increasing clockwise as on the real wheel */
static int tw_position(int x, int y)
{
    int pos = lround(atan2(y - tw.y, x - tw.x)
                     * TW_CLICKS_PER_ROTATION / (2 * M_PI));
    return (pos + TW_CLICKS_PER_ROTATION) % TW_CLICKS_PER_ROTATION;
}

static int tw_key_at(int x, int y)
{
    int dx = x - tw.x, dy = y - tw.y;
    if (abs(dy) >= abs(dx))
        return tw.keys[dy < 0 ? 0 : 2];
    return tw.keys[dx > 0 ? 1 : 3];
}

static void tw_release_tap(void)
{
    if (tw_tap_key)
    {
        button_event(tw_tap_key, false);
        tw_tap_key = 0;
    }
}

static bool touchwheel_down(int x, int y)
{
    if (tw_event_type == (Uint32)-1 || !tw_on_ring(x, y))
        return false;

    tw_release_tap();
    tw_gen++;
    tw_state = TW_TOUCHED;
    tw_key = tw_key_at(x, y);
    tw_pos = tw_position(x, y);
    tw_start_timer(TW_TIMER_HOLD, TW_HOLD_MS);
    return true;
}

static void touchwheel_motion(int x, int y, Uint32 ms)
{
    if (tw_state != TW_TOUCHED && tw_state != TW_SCROLLING)
        return;
    /* the angle is meaningless near the centre */
    if (tw_dist2(x, y) * 4 < tw.inner_radius * tw.inner_radius)
        return;

    int pos = tw_position(x, y);
    int delta = pos - tw_pos;
    if (delta < -TW_CLICKS_PER_ROTATION/2)
        delta += TW_CLICKS_PER_ROTATION;
    else if (delta > TW_CLICKS_PER_ROTATION/2)
        delta -= TW_CLICKS_PER_ROTATION;
    if (abs(delta) < TW_SENSITIVITY)
        return;

    int dir = delta > 0 ? BUTTON_SCROLL_FWD : BUTTON_SCROLL_BACK;
    int repeat = 0;
    Uint32 dt = ms - tw_last_ms;

    /* degrees/sec, smoothed like the clickwheel driver */
    unsigned v = dt ? (1000u * abs(delta) / dt) * 360 / TW_CLICKS_PER_ROTATION
                    : 0;
    tw_velocity = (15 * tw_velocity + v) / 16;
    if (tw_velocity > 0xffffff)
        tw_velocity = 0xffffff;

    /* direction reversals and pauses reset acceleration */
    if (tw_state != TW_SCROLLING || dir != tw_dir || dt > TW_FAST_OFF_MS)
        tw_velocity = 0;
    else if (dt < TW_REPEAT_MS)
        repeat = BUTTON_REPEAT;

    tw_state = TW_SCROLLING;
    tw_dir = dir;
    tw_pos = pos;
    tw_last_ms = ms;

#ifdef HAVE_BACKLIGHT
    backlight_on();
#endif
    reset_poweroff_timer();
    if (button_queue_empty())
        button_queue_post(dir | repeat, (1u << 31) | (1 << 24) | tw_velocity);
}

static bool touchwheel_up(void)
{
    switch (tw_state)
    {
    case TW_IDLE:
        return false;
    case TW_TOUCHED:
        /* quick tap: click briefly, long enough to pass debouncing */
        tw_tap_key = tw_key;
        button_event(tw_tap_key, true);
        tw_start_timer(TW_TIMER_TAP_RELEASE, TW_TAP_MS);
        break;
    case TW_PRESSED:
        button_event(tw_key, false);
        break;
    case TW_SCROLLING:
        break;
    }
    tw_state = TW_IDLE;
    return true;
}

static void touchwheel_timer(int code)
{
    if ((unsigned)code >> 1 != (tw_gen & (UINT_MAX >> 1)))
        return; /* from an earlier touch */

    if ((code & 1) == TW_TIMER_TAP_RELEASE)
        tw_release_tap();
    else if (tw_state == TW_TOUCHED)
    {
        /* finger held still: click and hold until it lifts */
        tw_state = TW_PRESSED;
        button_event(tw_key, true);
    }
}
#endif /* HAVE_SIM_TOUCHWHEEL */

static void mouse_event(SDL_MouseButtonEvent *event, bool button_up)
{
#define SQUARE(x) ((x)*(x))
    static int x,y;
#ifdef SIMULATOR
    static int xybutton = 0;
#endif

    if(button_up) {
        switch ( event->button )
        {
        case SDL_BUTTON_MIDDLE:
        case SDL_BUTTON_RIGHT:
            button_event( event->button, false );
            break;
        /* The scrollwheel button up events are ignored as they are queued immediately */
        case SDL_BUTTON_LEFT:
#ifdef HAVE_SIM_TOUCHWHEEL
            if (touchwheel_up())
                break;
#endif
            if ( mapping && background ) {
                printf("    { SDLK_,     %d, %d, %d, \"\" },\n", x, y,
                (int)sqrt( SQUARE(x-(int)event->x) + SQUARE(y-(int)event->y))
                );
            }
#ifdef SIMULATOR
            if ( background && xybutton ) {
                    button_event( xybutton, false );
                    xybutton = 0;
                }
#endif
#ifdef HAVE_TOUCHSCREEN
                else
                    button_event(BUTTON_TOUCHSCREEN, false);
#endif
            break;
        }
    } else {   /* button down */
        switch ( event->button )
        {
        case SDL_BUTTON_MIDDLE:
        case SDL_BUTTON_RIGHT:
            button_event( event->button, true );
            break;
        case SDL_BUTTON_LEFT:
            if ( mapping && background ) {
                x = event->x;
                y = event->y;
            }
#ifdef HAVE_SIM_TOUCHWHEEL
            if (touchwheel_down(event->x, event->y))
                break;
#endif
#ifdef SIMULATOR
            if ( background ) {
                xybutton = xy2button( event->x, event->y );
                if( xybutton ) {
                    button_event( xybutton, true );
                    break;
                }
            }
#endif
#ifdef HAVE_TOUCHSCREEN
            touchscreen_event(event->x, event->y);
#endif
            break;
        }

        if (debug_wps && event->button == SDL_BUTTON_LEFT)
        {
            int m_x, m_y;

            if ( background )
            {
                m_x = event->x - 1;
                m_y = event->y - 1;
#ifdef HAVE_REMOTE
                if ( event->y >= UI_REMOTE_POSY ) /* Remote Screen */
                {
                    m_x -= UI_REMOTE_POSX;
                    m_y -= UI_REMOTE_POSY;
                }
                else
#endif
                {
                    m_x -= UI_LCD_POSX;
                    m_y -= UI_LCD_POSY;
                }
            }
            else
            {
                m_x = event->x;
                m_y = event->y;
#ifdef HAVE_REMOTE
                if ( m_y >= LCD_HEIGHT ) /* Remote Screen */
                    m_y -= LCD_HEIGHT;
#endif
            }

            printf("Mouse at 2: (%d, %d)\n", m_x, m_y);
        }
    }
#undef SQUARE
}

static bool event_handler(SDL_Event *event)
{
#if SDL_MAJOR_VERSION > 1
    SDL_Keycode ev_key;
#else
    SDLKey ev_key;
#endif

#ifdef HAVE_SIM_TOUCHWHEEL
    if (event->type == tw_event_type && event->user.data1 == &tw_event_type)
    {
        touchwheel_timer(event->user.code);
        return false;
    }
#endif

    switch(event->type)
    {
#if SDL_MAJOR_VERSION > 1
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            sdl_app_has_input_focus = 1;
        else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            sdl_app_has_input_focus = 0;
        else if(event->window.event == SDL_WINDOWEVENT_RESIZED)
        {
            SDL_LockMutex(window_mutex);
            sdl_window_adjustment_needed(false);
            SDL_UnlockMutex(window_mutex);
#if !defined (__APPLE__) && !defined(__WIN32)
            static unsigned long last_tick;
            if (TIME_AFTER(current_tick, last_tick + HZ/20) && !button_queue_full())
                  button_queue_post(SDLK_UNKNOWN, 0); /* update window on main thread */
            else
                  last_tick = current_tick;
#endif
        }
#endif /* SDL_MAJOR_VERSION */
#ifdef SIMULATOR
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST
            || event->window.event == SDL_WINDOWEVENT_LEAVE
            || event->window.event == SDL_WINDOWEVENT_RESIZED
            || event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
        {
            if (cursor_isfocus)
            {
                cursor_isfocus = false;
                SDL_SetCursor(sdl_arrow_cursor);
            }
        }
#endif
        break;
#ifdef __ANDROID__
    case SDL_APP_DIDENTERBACKGROUND:
        sdl_window_set_visible(false);
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        SDL_LockMutex(window_mutex);
        sdl_window_set_visible(true);
        sdl_window_adjustment_needed(false);
        SDL_UnlockMutex(window_mutex);
        if (!button_queue_full())
            button_queue_post(SDLK_UNKNOWN, 0); /* redraw on main thread */
        break;
#endif
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        ev_key = event->key.keysym.sym;
        button_event(ev_key, event->type == SDL_KEYDOWN);
        break;



    case SDL_MOUSEMOTION:
    {
#ifdef SIMULATOR
        static uint32_t next_check = 0;
        if (background && sdl_app_has_input_focus
            && (TIME_AFTER(event->motion.timestamp, next_check)))
        {
            int x = event->motion.x;
            int y = event->motion.y;

            extern struct button_map bm[];
            int i;
            for (i = 0; bm[i].button; i++)
            {
                int xd = (x-bm[i].x)*(x-bm[i].x);
                int yd = (y-bm[i].y)*(y-bm[i].y);
                /* check distance from center of button < radius */
                if ( xd + yd < bm[i].radius*bm[i].radius ) {
                    break;
                }
            }

            if (bm[i].button
#ifdef HAVE_SIM_TOUCHWHEEL
                || tw_on_ring(x, y)
#endif
               )
            {
                if (!cursor_isfocus)
                {
                    cursor_isfocus = true;
                    SDL_SetCursor(sdl_focus_cursor);
                }
            }
            else if (cursor_isfocus)
            {
                cursor_isfocus = false;
                SDL_SetCursor(sdl_arrow_cursor);
            }

            next_check = event->motion.timestamp + 10; /* ms */
        }
#endif
#ifdef HAVE_SIM_TOUCHWHEEL
        touchwheel_motion(event->motion.x, event->motion.y,
                          event->motion.timestamp);
#endif
#ifdef HAVE_TOUCHSCREEN
        if (event->motion.state & SDL_BUTTON(1))
        {
            int x = event->motion.x;
            int y = event->motion.y;
            touchscreen_event(x, y);
        }
#endif
        break;

    }
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEBUTTONDOWN:
    {
        SDL_MouseButtonEvent *mev = &event->button;
        mouse_event(mev, event->type == SDL_MOUSEBUTTONUP);
        break;
    }
#ifdef HAVE_SCROLLWHEEL
    case SDL_MOUSEWHEEL:
    {
        scrollwheel_event(event->wheel.x, event->wheel.y);
        break;
    }
#endif
    case SDL_QUIT:
        /* Will post SDL_USEREVENT in shutdown_hw() if successful. */
        sdl_sys_quit();
        break;
    case SDL_USEREVENT:
        return true;
        break;
    }

    return false;
}

#ifdef __APPLE__
int sdl_event_filter(void *userdata, SDL_Event * event)
{
    (void) userdata;
    event_handler(event);
    return 0;
}
#endif

void gui_message_loop(void)
{
    SDL_Event event;
    bool quit;

    do {
        /* wait for the next event */
        if(SDL_WaitEvent(&event) == 0) {
            printf("SDL_WaitEvent(): %s\n", SDL_GetError());
            return; /* error, out of here */
        }

        sim_enter_irq_handler();
        quit = event_handler(&event);
        sim_exit_irq_handler();

    } while(!quit);
}

#if defined(SIMULATOR)
static void show_sim_help(void)
{
    extern SDL_Window *sdlWindow;
    extern struct button_map bm[];

    uint32_t flags = SDL_MESSAGEBOX_INFORMATION;
    static char helptext[4096] = {'\0'};

    if (helptext[0] != '\0')
    {
        SDL_ShowSimpleMessageBox(flags, UI_TITLE, helptext, sdlWindow);
        return;
    }

    const char *keyname = SDL_GetKeyName(SDLK_HELP);
    strlcat(helptext, "Rockbox simulator\n[Key] Description\n\n[", sizeof(helptext)-1);
    /* simulator keys */
    #define HELPTXT(SDLK,litdesc)                       \
        keyname = SDL_GetKeyName(SDLK);                 \
        if (keyname[0]) {                               \
        strlcat(helptext, keyname, sizeof(helptext)-1); \
        strlcat(helptext, "] "litdesc"\n[", sizeof(helptext)-1);}
    /*HELPTXT(, "");*/
    HELPTXT(SDLK_HELP, "display this window");
    HELPTXT(SDLK_KP_0, "screendump");
    HELPTXT(SDLK_F5, "screendump");
    HELPTXT(SDLK_TAB, "hide device background");
    HELPTXT(SDLK_0, "zoom display level 1/2");
    HELPTXT(SDLK_1, "zoom display level 1");
    HELPTXT(SDLK_2, "zoom display level 2");
    HELPTXT(SDLK_3, "zoom display level 3");
    HELPTXT(SDLK_4, "toggle display quality");
    HELPTXT(USB_KEY, "toggle USB");

#ifdef HAVE_HEADPHONE_DETECTION
    HELPTXT(SDLK_p, "toggle headphone");
#endif
#ifdef HAVE_LINEOUT_DETECTION
    HELPTXT(SDLK_p, "toggle lineout");
#endif
#ifdef HAVE_HOTSWAP
    HELPTXT(EXT_KEY, "toggle external drive");
#endif
#ifdef HAS_BUTTON_HOLD
    HELPTXT(SDLK_h, "toggle hold button");
#endif
#ifdef HAS_REMOTE_BUTTON_HOLD
    HELPTXT(SDLK_j, "toggle remote hold button");
#endif
#if defined(IRIVER_H100_SERIES) || defined (IRIVER_H300_SERIES)
    HELPTXT(SDLK_t, "toggle remote type");
#endif
#ifdef HAVE_TOUCHSCREEN
    HELPTXT(SDLK_F4, "toggle touch mode");
#endif

    /* device specific keymap */
    for (int i = 0; bm[i].button; i++)
    {
        keyname = SDL_GetKeyName(bm[i].button);
        strlcat(helptext, keyname, sizeof(helptext)-1);
        strlcat(helptext, "] ", sizeof(helptext)-1);
        strlcat(helptext, bm[i].description, sizeof(helptext)-1);
        strlcat(helptext, "\n[", sizeof(helptext)-1);
    }

    /* last one doesn't use the macro */
    keyname = SDL_GetKeyName(SDLK_F1);
    strlcat(helptext, keyname, sizeof(helptext)-1);
    strlcat(helptext, "] display this window\n\n", sizeof(helptext)-1);

strlcat(helptext, "Note: If you don't have a keypad\n" \
                  "alternate keys are in uisimulator/buttonmap\n\n", sizeof(helptext)-1);

    DEBUGF("[%u] characters\n%s", (unsigned int)strlen(helptext), helptext);
    SDL_ShowSimpleMessageBox(flags, UI_TITLE, helptext, sdlWindow);
}
#endif

static void button_event(int key, bool pressed)
{
    int new_btn = 0;
    switch (key)
    {
#ifdef SIMULATOR
    case SDLK_HELP:
    case SDLK_F1:
        if (!pressed)
            show_sim_help();
        return;
    case SDLK_TAB:
        if (!pressed)
        {
            SDL_LockMutex(window_mutex);
            background = !background;
            sdl_window_adjustment_needed(true);
            SDL_UnlockMutex(window_mutex);
#if !defined(__WIN32) && !defined (__APPLE__)
            button_queue_post(SDLK_UNKNOWN, 0); /* update window on main thread */
#endif
        }
        return;
#ifdef HAVE_SIM_TOUCHWHEEL
    case SIM_SKIN_COLOUR_KEY:
        if (!pressed)
        {
            SDL_LockMutex(window_mutex);
            sdl_window_cycle_skin_colour();
            sdl_window_adjustment_needed(true);
            SDL_UnlockMutex(window_mutex);
#if !defined(__WIN32) && !defined (__APPLE__)
            button_queue_post(SDLK_UNKNOWN, 0); /* update window on main thread */
#endif
        }
        return;
#endif
    case SDLK_0:
        display_zoom = 0.5;
    case SDLK_1:
        display_zoom = display_zoom ?: 1;
    case SDLK_2:
        display_zoom = display_zoom ?: 2;
    case SDLK_3:
        display_zoom = display_zoom ?: 3;
    case SDLK_4:
        if (pressed)
        {
            display_zoom = 0;
            return;
        }
        SDL_LockMutex(window_mutex);
        if (!display_zoom)
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                        strcmp(SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY) ?:
                        "best", "best") ? "best": "nearest");

        sdl_window_adjustment_needed(!display_zoom);
        SDL_UnlockMutex(window_mutex);
#if !defined(__WIN32) && !defined (__APPLE__)
        button_queue_post(SDLK_UNKNOWN, 0); /* update window on main thread */
#endif
        return;
    case USB_KEY:
        if (!pressed)
        {
            static bool usb_connected = false;
            usb_connected = !usb_connected;
            sim_trigger_usb(usb_connected);
        }
        return;
#ifdef HAVE_HEADPHONE_DETECTION
    case SDLK_p:
        if (!pressed)
        {
            static bool hp_connected = true;
            hp_connected = !hp_connected;
            sim_trigger_hp(hp_connected);
        }
        return;
#endif
#ifdef HAVE_LINEOUT_DETECTION
    case SDLK_l:
        if (!pressed)
        {
            static bool lo_connected = false;
            lo_connected = !lo_connected;
            sim_trigger_lo(lo_connected);
        }
        return;
#endif
#ifdef HAVE_HOTSWAP
    case EXT_KEY:
        if (!pressed)
            sim_trigger_external(!storage_present(1));
        return;
#endif
#endif
#ifdef RG_NANO
    case SDLK_q:
        /* Use reboot to exit without shutting down */
        sys_reboot();
        break;
#endif
#ifdef HAS_BUTTON_HOLD
    case SDLK_h:
        if(pressed)
        {
            hold_button_state = !hold_button_state;
            DEBUGF("Hold button is %s\n", hold_button_state?"ON":"OFF");
        }
        return;
#endif

#ifdef HAS_REMOTE_BUTTON_HOLD
    case SDLK_j:
        if(pressed)
        {
            remote_hold_button_state = !remote_hold_button_state;
            DEBUGF("Remote hold button is %s\n",
                   remote_hold_button_state?"ON":"OFF");
        }
        return;
#endif

#if defined(IRIVER_H100_SERIES) || defined (IRIVER_H300_SERIES)
    case SDLK_t:
        if(pressed)
            switch(_remote_type)
            {
                case REMOTETYPE_UNPLUGGED:
                    _remote_type=REMOTETYPE_H100_LCD;
                    DEBUGF("Changed remote type to H100\n");
                    break;
                case REMOTETYPE_H100_LCD:
                    _remote_type=REMOTETYPE_H300_LCD;
                    DEBUGF("Changed remote type to H300\n");
                    break;
                case REMOTETYPE_H300_LCD:
                    _remote_type=REMOTETYPE_H300_NONLCD;
                    DEBUGF("Changed remote type to H300 NON-LCD\n");
                    break;
                case REMOTETYPE_H300_NONLCD:
                    _remote_type=REMOTETYPE_UNPLUGGED;
                    DEBUGF("Changed remote type to none\n");
                    break;
            }
        break;
#endif
#ifndef APPLICATION
    case SDLK_KP_0:
    case SDLK_F5:
        if(pressed)
        {
            sim_trigger_screendump();
            return;
        }
        break;
#endif
#ifdef HAVE_TOUCHSCREEN
    case SDLK_F4:
        if(pressed)
        {
            touchscreen_set_mode(touchscreen_get_mode() == TOUCHSCREEN_POINT ? TOUCHSCREEN_BUTTON : TOUCHSCREEN_POINT);
            printf("Touchscreen mode: %s\n", touchscreen_get_mode() == TOUCHSCREEN_POINT ? "TOUCHSCREEN_POINT" : "TOUCHSCREEN_BUTTON");
        }
#endif
    default:
#ifdef HAVE_TOUCHSCREEN
# ifndef HAS_BUTTON_HOLD
        if(touchscreen_is_enabled())
# endif
            new_btn = key_to_touch(key, mouse_coords);
        if (!new_btn)
#endif
            new_btn = key_to_button(key);
#ifdef HAVE_TOUCHPAD
        new_btn = touchpad_filter(new_btn);
#endif
        break;
    }

#if defined(BUTTON_SCROLL_FWD) && defined(BUTTON_SCROLL_BACK)
    if((new_btn == BUTTON_SCROLL_FWD || new_btn == BUTTON_SCROLL_BACK) &&
        pressed)
    {
        scrollwheel_event(0, new_btn == BUTTON_SCROLL_FWD ? -1 : 1);
        new_btn &= ~(BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK);
    }
#endif

    /* Update global button press state */
    if (pressed)
        btn |= new_btn;
    else
        btn &= ~new_btn;
}
#if defined(HAVE_BUTTON_DATA)
int button_read_device(int* data)
{
#if defined(HAVE_TOUCHSCREEN)
    *data = mouse_coords;
#else
    (void) *data;   /* suppress compiler warnings */
#endif
#else
int button_read_device(void)
{
#endif
#ifdef HAS_BUTTON_HOLD
    int hold_button = button_hold();

#ifdef HAVE_BACKLIGHT
    /* light handling */
    static int hold_button_old = false;
    if (hold_button != hold_button_old)
    {
        hold_button_old = hold_button;
        backlight_hold_changed(hold_button);
    }
#endif

    if (hold_button)
        return BUTTON_NONE;
    else
#endif

    return btn;
}

void button_init_device(void)
{
#ifdef SIMULATOR
    if (!sdl_focus_cursor)
        sdl_focus_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    if (!sdl_arrow_cursor)
        sdl_arrow_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
#endif
#ifdef HAVE_SIM_TOUCHWHEEL
    if (tw_event_type == (Uint32)-1)
        tw_event_type = SDL_RegisterEvents(1);
#endif
}
