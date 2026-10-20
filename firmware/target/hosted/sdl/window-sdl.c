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

#include <SDL.h>
#include <stdio.h>
#include "sim-ui-defines.h"
#include "window-sdl.h"
#include "buttonmap.h"
#include "lcd-sdl.h"
#include "misc.h"
#include "panic.h"

extern SDL_Surface *lcd_surface;
#ifdef HAVE_REMOTE_LCD
extern SDL_Surface *remote_surface;
#endif

SDL_Texture  *gui_texture;
SDL_Surface  *sim_lcd_surface;

SDL_mutex *window_mutex;

SDL_Window   *sdlWindow;
static SDL_Renderer *sdlRenderer;
static SDL_Surface  *picture_surface;

#ifdef HAVE_SIM_TOUCHWHEEL
/* Colours for the device body and centre button; the clickwheel ring and
   the screen keep theirs. The first keeps the image as it is. */
static const struct { Uint8 r, g, b; } skin_colours[] = {
    {   0,   0,   0 }, /* original */
    {  36,  36,  40 }, /* black */
    { 196, 200, 206 }, /* silver */
    { 242, 160, 190 }, /* pink */
    { 120, 170, 230 }, /* blue */
    { 150, 210, 120 }, /* green */
    { 200,  30,  40 }, /* red */
};
#define SKIN_COLOUR_FILE "skincolour.txt"
static int skin_colour;
static SDL_Surface *skin_image; /* as loaded, picture_surface is tinted */

static int luminance(Uint8 r, Uint8 g, Uint8 b)
{
    return (r * 299 + g * 587 + b * 114) / 1000;
}

/* The ring's light bevel takes the body colour too, or it shows as an
   outline on dark colours */
#define TINT_BEVEL_INNER 1
#define TINT_BEVEL_OUTER 2

/* Keep some of the original skin showing through the colour */
#define TINT_STRENGTH 0.8f

/* How much of the pixel belongs to the body or centre button: 0 on the
   ring and the screen, blended across the ring's edges */
static float tint_weight(int x, int y)
{
    const float inner = tw.inner_radius + TINT_BEVEL_INNER;
    const float outer = tw.outer_radius - TINT_BEVEL_OUTER;
    if (x >= UI_LCD_POSX && x < UI_LCD_POSX + SIM_LCD_WIDTH &&
        y >= UI_LCD_POSY && y < UI_LCD_POSY + SIM_LCD_HEIGHT)
        return 0;
    float d = SDL_sqrtf((x - tw.x) * (x - tw.x) + (y - tw.y) * (y - tw.y));
    float w = d < inner ? inner - d + 0.5f : d - outer + 0.5f;
    return w < 0 ? 0 : w > 1 ? 1 : w;
}

static void apply_skin_colour(void)
{
    SDL_Surface *s = SDL_ConvertSurfaceFormat(skin_image, SDL_PIXELFORMAT_RGB888, 0);
    if (!s)
        return;

    if (skin_colour && tw.outer_radius)
    {
        Uint32 *px = s->pixels;
        int stride = s->pitch / 4;
        Uint8 r, g, b;

        /* The body's average brightness maps to the colour itself */
        long sum = 0, count = 0;
        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++)
            {
                SDL_GetRGB(px[y * stride + x], s->format, &r, &g, &b);
                int l = luminance(r, g, b);
                if (tint_weight(x, y) == 1 && l > 64)
                {
                    sum += l;
                    count++;
                }
            }
        float ref = count ? (float)sum / count : 255;

        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++)
            {
                float w = tint_weight(x, y) * TINT_STRENGTH;
                if (w == 0)
                    continue;
                Uint32 *p = &px[y * stride + x];
                SDL_GetRGB(*p, s->format, &r, &g, &b);
                float k = luminance(r, g, b) / ref;
                float tr = SDL_min(255.f, skin_colours[skin_colour].r * k);
                float tg = SDL_min(255.f, skin_colours[skin_colour].g * k);
                float tb = SDL_min(255.f, skin_colours[skin_colour].b * k);
                *p = SDL_MapRGB(s->format, r + (tr - r) * w,
                                g + (tg - g) * w, b + (tb - b) * w);
            }
    }

    if (picture_surface)
        SDL_FreeSurface(picture_surface);
    picture_surface = s;
}

void sdl_window_cycle_skin_colour(void)
{
    if (!skin_image)
        return;
    skin_colour = (skin_colour + 1) % (int)SDL_arraysize(skin_colours);
    apply_skin_colour();

    FILE *f = fopen(SKIN_COLOUR_FILE, "w");
    if (f)
    {
        fprintf(f, "%d\n", skin_colour);
        fclose(f);
    }
}

static void load_skin_colour(void)
{
    FILE *f = fopen(SKIN_COLOUR_FILE, "r");
    if (f)
    {
        if (fscanf(f, "%d", &skin_colour) != 1 || skin_colour < 0 ||
            skin_colour >= (int)SDL_arraysize(skin_colours))
            skin_colour = 0;
        fclose(f);
    }
    apply_skin_colour();
}
#endif /* HAVE_SIM_TOUCHWHEEL */

static bool new_gui_texture_needed = true;
static bool window_adjustment_needed;
double display_zoom = 1;

static void get_window_dimensions(int *w, int *h)
{
    if (background)
    {
        *w = UI_WIDTH;
        *h = UI_HEIGHT;
    }
    else
    {
#ifdef HAVE_REMOTE_LCD
        if (showremote)
        {
            *w = SIM_LCD_WIDTH > SIM_REMOTE_WIDTH ? SIM_LCD_WIDTH : SIM_REMOTE_WIDTH;
            *h = SIM_LCD_HEIGHT + SIM_REMOTE_HEIGHT;
        }
        else
#endif
        {
            *w = SIM_LCD_WIDTH;
            *h = SIM_LCD_HEIGHT;
        }
    }
}

#if defined(__APPLE__) || defined(__WIN32)
static void restore_aspect_ratio(int w, int h)
{
    float aspect_ratio = (float) h / w;
    int original_height = h;
    int original_width = w;

    if ((SDL_GetWindowFlags(sdlWindow) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))
        || display_zoom)
        return;

    SDL_GetWindowSize(sdlWindow, &w, &h);
    if (w != original_width || h != original_height)
    {
        SDL_DisplayMode sdl_dm;
        h = w * aspect_ratio;
        if (SDL_GetCurrentDisplayMode(0, &sdl_dm) || h <= sdl_dm.h)
            SDL_SetWindowSize(sdlWindow, w, h);
    }
}
#endif

static void rebuild_gui_texture(void)
{
    SDL_Surface *gui_surface;
    int prev_w, prev_h, x, y,
        w, h, depth = LCD_DEPTH < 8 ? 16 : LCD_DEPTH;
    Uint32 flags = SDL_GetWindowFlags(sdlWindow);

    get_window_dimensions(&w, &h);
    SDL_RenderGetLogicalSize(sdlRenderer, &prev_w, &prev_h);
    SDL_RenderSetLogicalSize(sdlRenderer, w, h);
    if ((gui_texture = SDL_CreateTexture(sdlRenderer, SDL_MasksToPixelFormatEnum(depth,
                                         0, 0, 0, 0), SDL_TEXTUREACCESS_STREAMING, w, h)) == NULL)
        panicf("%s", SDL_GetError());

    /* Did background change? */
    if ((flags & SDL_WINDOW_RESIZABLE) &&
        !(flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) &&
        prev_w && prev_w != w)
    {
        SDL_GetWindowSize(sdlWindow, &x, NULL);

        /* Maintain LCD's size */
        float ratio = (float) x / prev_w;
        SDL_SetWindowSize(sdlWindow, w * ratio, h * ratio);

        /* move LCD back into previous position */
        SDL_GetWindowPosition(sdlWindow, &x, &y);
        if (background)
        {
            x -= (UI_LCD_POSX * ratio);
            y -= (UI_LCD_POSY * ratio);
        }
        else
        {
            x += (UI_LCD_POSX * ratio);
            y += (UI_LCD_POSY * ratio);
        }
        SDL_SetWindowPosition(sdlWindow, x > 0 ? x : 0, y > 0 ? y : 0);
    }

    if (background && picture_surface &&
        (gui_surface = SDL_ConvertSurface(picture_surface, sim_lcd_surface->format, 0)))
    {
        SDL_UpdateTexture(gui_texture, NULL, gui_surface->pixels, gui_surface->pitch);
        SDL_FreeSurface(gui_surface);
    }

    sdl_gui_update(lcd_surface, 0, 0, SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
                   SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
                   background ? UI_LCD_POSX : 0, background? UI_LCD_POSY : 0);

#ifdef HAVE_REMOTE_LCD
    sdl_gui_update(remote_surface, 0, 0, LCD_REMOTE_WIDTH, LCD_REMOTE_HEIGHT,
                   LCD_REMOTE_WIDTH, LCD_REMOTE_HEIGHT,
                   background ? UI_REMOTE_POSX : 0,
                   background? UI_REMOTE_POSY : LCD_HEIGHT);
#endif
}

#ifdef __ANDROID__
static volatile bool window_hidden = false;
static volatile bool surface_replaced = false;

void sdl_window_set_visible(bool visible)
{
    if (visible)
        surface_replaced = true;
    window_hidden = !visible;
}
#endif

void sdl_window_render(void)
{
#ifdef __ANDROID__
    if (window_hidden)
        return;
    if (surface_replaced)
    {
        /* SDL restores the GL context on the event thread, but it is
           current on this one: rebind it to the new surface */
        SDL_GLContext context = SDL_GL_GetCurrentContext();
        surface_replaced = false;
        SDL_GL_MakeCurrent(sdlWindow, NULL);
        SDL_GL_MakeCurrent(sdlWindow, context);
    }
#endif
    if (new_gui_texture_needed)
    {
        new_gui_texture_needed = false;
        if (gui_texture)
            SDL_DestroyTexture(gui_texture);
        rebuild_gui_texture();
    }
    else
    {
        SDL_RenderClear(sdlRenderer);
        SDL_RenderCopy(sdlRenderer, gui_texture, NULL, NULL);
        SDL_RenderPresent(sdlRenderer);
    }
}

bool sdl_window_adjust(void)
{
    int w, h;

    if (!window_adjustment_needed)
        return false;
    window_adjustment_needed = false;

    get_window_dimensions(&w, &h);

    if (!(SDL_GetWindowFlags(sdlWindow) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))
        && display_zoom)
    {
        SDL_SetWindowSize(sdlWindow, display_zoom * w, display_zoom * h);
    }
#if defined(__APPLE__) || defined(__WIN32)
    int logical_w, logical_h;
    SDL_RenderGetLogicalSize(sdlRenderer, &logical_w, &logical_h);
    if (logical_w == w && logical_h == h) /* background unchanged */
        restore_aspect_ratio(w, h);
#endif
    display_zoom = 0;
    sdl_window_render();
    return true;
}

void sdl_window_adjustment_needed(bool destroy_texture)
{
    window_adjustment_needed = true;
    new_gui_texture_needed = destroy_texture;

    /* For MacOS and Windows, we're on a main or
    display thread already, and can immediately
    adjust the window.
    On Linux, we have to defer the update, until
    it is handled by the main thread later. */
#if defined (__APPLE__) || defined(__WIN32)
    sdl_window_adjust();
#endif
}

void sdl_window_setup(void)
{
    int width, height;
    int depth = LCD_DEPTH < 8 ? 16 : LCD_DEPTH;
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;

#ifdef __ANDROID__
    /* Hides the status and navigation bars; resizable lets SDL allow
       any orientation, honouring the rotation lock */
    flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE;
#else
    if (display_zoom == 1)
        flags |= SDL_WINDOW_RESIZABLE;
#endif

    if (!(picture_surface = SDL_LoadBMP("UI256.bmp")))
        background = false;
#ifdef HAVE_SIM_TOUCHWHEEL
    else
    {
        skin_image = picture_surface;
        picture_surface = NULL;
        load_skin_colour();
        if (!picture_surface)
        {
            picture_surface = skin_image;
            skin_image = NULL;
        }
    }
#endif

    get_window_dimensions(&width, &height);

    if ((sdlWindow = SDL_CreateWindow(UI_TITLE, SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED, width * display_zoom,
                                   height * display_zoom , flags)) == NULL)
        panicf("%s", SDL_GetError());
    if ((sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC)) == NULL)
        panicf("%s", SDL_GetError());

    /* Surface for LCD content only. Needs to fit largest LCD */
    if ((sim_lcd_surface = SDL_CreateRGBSurface(0,
#ifdef HAVE_REMOTE_LCD
                                                SIM_LCD_WIDTH > SIM_REMOTE_WIDTH ?
                                                SIM_LCD_WIDTH : SIM_REMOTE_WIDTH,
                                                SIM_LCD_HEIGHT > SIM_REMOTE_HEIGHT ?
                                                SIM_LCD_HEIGHT : SIM_REMOTE_HEIGHT,
#else
                                                SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
#endif
                                                depth, 0, 0, 0, 0)) == NULL)
        panicf("%s", SDL_GetError());

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, display_zoom == 1 ? "best" : "nearest");
    display_zoom = 0; /* reset to 0 unless/until user requests a scale level change */
    window_mutex = SDL_CreateMutex();
}
