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
/* Colour for the device body and centre button; the clickwheel ring and
   the screen keep theirs. Opacity 0 keeps the image as it is. */
static struct { int r, g, b, opacity; } skin_tint = { 150, 100, 200, 0 };
#define SKIN_COLOUR_FILE "skincolour.txt"
static SDL_Surface *skin_image; /* as loaded, picture_surface is tinted */

#ifndef __ANDROID__
/* Colours the skin colour key cycles through: each light, normal and dark
   after the original image (preset 0). Android has a colour picker. */
static const struct { Uint8 r, g, b; } skin_presets[] = {
    {  36,  36,  40 }, /* black */
    { 196, 200, 206 }, /* silver */
    { 242, 160, 190 }, /* pink */
    { 120, 170, 230 }, /* blue */
    { 150, 210, 120 }, /* green */
    { 200,  30,  40 }, /* red */
    { 150, 100, 200 }, /* purple */
    { 240, 210,  80 }, /* yellow */
    { 240, 140,  60 }, /* orange */
    { 215, 185, 120 }, /* gold */
    {  70, 175, 175 }, /* teal */
};
#define SKIN_SHADES 3 /* light, normal, dark */
#define SKIN_PRESETS (1 + SKIN_SHADES * (int)SDL_arraysize(skin_presets))
#define SKIN_PRESET_OPACITY 80
static int skin_preset;

static void set_skin_preset(void)
{
    if (skin_preset == 0)
    {
        skin_tint.opacity = 0;
        return;
    }
    int shade = (skin_preset - 1) % SKIN_SHADES;
    int i = (skin_preset - 1) / SKIN_SHADES;
    float r = skin_presets[i].r, g = skin_presets[i].g, b = skin_presets[i].b;
    if (shade == 0) /* light: towards white */
    {
        r += (255 - r) * 0.35f;
        g += (255 - g) * 0.35f;
        b += (255 - b) * 0.35f;
    }
    else if (shade == 2) /* dark */
    {
        r *= 0.65f;
        g *= 0.65f;
        b *= 0.65f;
    }
    skin_tint.r = r;
    skin_tint.g = g;
    skin_tint.b = b;
    skin_tint.opacity = SKIN_PRESET_OPACITY;
}
#endif /* !__ANDROID__ */

static int luminance(Uint8 r, Uint8 g, Uint8 b)
{
    return (r * 299 + g * 587 + b * 114) / 1000;
}

/* The ring's light bevel takes the body colour too, or it shows as an
   outline on dark colours */
#define TINT_BEVEL_INNER 1
#define TINT_BEVEL_OUTER 2

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

    if (skin_tint.opacity > 0 && tw.outer_radius)
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
        float opacity = skin_tint.opacity / 100.f;

        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++)
            {
                float w = tint_weight(x, y) * opacity;
                if (w == 0)
                    continue;
                Uint32 *p = &px[y * stride + x];
                SDL_GetRGB(*p, s->format, &r, &g, &b);
                float k = luminance(r, g, b) / ref;
                float tr = SDL_min(255.f, skin_tint.r * k);
                float tg = SDL_min(255.f, skin_tint.g * k);
                float tb = SDL_min(255.f, skin_tint.b * k);
                *p = SDL_MapRGB(s->format, r + (tr - r) * w,
                                g + (tg - g) * w, b + (tb - b) * w);
            }
    }

    if (picture_surface)
        SDL_FreeSurface(picture_surface);
    picture_surface = s;
}

static void save_skin_colour(void)
{
    FILE *f = fopen(SKIN_COLOUR_FILE, "w");
    if (f)
    {
        fprintf(f, "%d %d %d %d\n", skin_tint.r, skin_tint.g, skin_tint.b,
                skin_tint.opacity);
        fclose(f);
    }
}

static void load_skin_colour(void)
{
    FILE *f = fopen(SKIN_COLOUR_FILE, "r");
    if (f)
    {
        int r, g, b, opacity;
        if (fscanf(f, "%d %d %d %d", &r, &g, &b, &opacity) == 4)
        {
            skin_tint.r = SDL_clamp(r, 0, 255);
            skin_tint.g = SDL_clamp(g, 0, 255);
            skin_tint.b = SDL_clamp(b, 0, 255);
            skin_tint.opacity = SDL_clamp(opacity, 0, 100);
        }
        fclose(f);
    }
    apply_skin_colour();
}

/* Vector device image (skin.svg, see uisimulator/android/skins), drawn
   sharp at the window's resolution. Runs of shapes whose group ids start
   with "body", "wheel" or "label" are drawn in white into their own
   layers and coloured when rendering, so colour changes are instant. */
#define SVG_SKIN_FILE "skin.svg"
#define SVG_MAX_LAYERS 16

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"
#pragma GCC diagnostic pop

enum { SVG_FIXED, SVG_BODY, SVG_WHEEL, SVG_LABEL };

static char *svg_skin; /* the SVG with its colours filled in white */
static struct
{
    SDL_Texture *texture;
    SDL_FRect dest; /* in device image coordinates */
    int tint;
} svg_layers[SVG_MAX_LAYERS];
static int svg_layer_count, svg_layers_w, svg_layers_h;

static int svg_tint(const char *id)
{
    if (!SDL_strncmp(id, "body", 4))
        return SVG_BODY;
    if (!SDL_strncmp(id, "wheel", 5) && SDL_strncmp(id, "wheel-details", 13))
        return SVG_WHEEL;
    if (!SDL_strncmp(id, "label", 5))
        return SVG_LABEL;
    return SVG_FIXED;
}

static void load_svg_skin(void)
{
    static const char *const colours[] =
        { "{body}", "{wheel}", "{button}", "{label}" };
    size_t size;
    char *text = SDL_LoadFile(SVG_SKIN_FILE, &size);
    if (!text)
        return;

    /* every placeholder becomes "#ffffff", one character longer */
    svg_skin = SDL_malloc(size * 2 + 1);
    char *out = svg_skin;
    for (const char *p = text; *p; )
    {
        size_t i;
        for (i = 0; i < SDL_arraysize(colours); i++)
            if (!SDL_strncmp(p, colours[i], SDL_strlen(colours[i])))
                break;
        if (i < SDL_arraysize(colours))
        {
            out += SDL_snprintf(out, 8, "#ffffff");
            p += SDL_strlen(colours[i]);
        }
        else
            *out++ = *p++;
    }
    *out = '\0';
    SDL_free(text);
}

static void free_svg_layers(void)
{
    for (int i = 0; i < svg_layer_count; i++)
        SDL_DestroyTexture(svg_layers[i].texture);
    svg_layer_count = 0;
}

/* Matte anodised aluminium: a faint fine grain, only darkening as the
   layer is white */
static void add_grain(Uint8 *px, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            Uint32 n = (Uint32)x * 374761393u + (Uint32)y * 668265263u;
            n = (n ^ (n >> 13)) * 1274126177u;
            float k = 1.f - ((n >> 16) & 0xff) / 255.f * 0.034f;
            Uint8 *p = &px[(y * w + x) * 4];
            p[0] *= k;
            p[1] *= k;
            p[2] *= k;
        }
}

static void build_svg_layers(int out_w, int out_h)
{
    free_svg_layers();
    svg_layers_w = out_w;
    svg_layers_h = out_h;

    char *text = SDL_strdup(svg_skin);
    NSVGimage *image = nsvgParse(text, "px", 96);
    SDL_free(text);
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!image || !rast)
        goto out;

    float scale = SDL_min(out_w / (float)UI_WIDTH, out_h / (float)UI_HEIGHT);
    NSVGshape *run = image->shapes;
    while (run && svg_layer_count < SVG_MAX_LAYERS)
    {
        int tint = svg_tint(run->id);
        float b[4] = { UI_WIDTH, UI_HEIGHT, 0, 0 };
        NSVGshape *end, *s;
        for (end = run; end && svg_tint(end->id) == tint; end = end->next)
        {
            /* bounds leave out strokes */
            float pad = end->strokeWidth / 2 + 1;
            b[0] = SDL_min(b[0], end->bounds[0] - pad);
            b[1] = SDL_min(b[1], end->bounds[1] - pad);
            b[2] = SDL_max(b[2], end->bounds[2] + pad);
            b[3] = SDL_max(b[3], end->bounds[3] + pad);
        }
        for (s = image->shapes; s; s = s->next)
            s->flags &= ~NSVG_FLAGS_VISIBLE;
        for (s = run; s != end; s = s->next)
            s->flags |= NSVG_FLAGS_VISIBLE;

        int x0 = SDL_max(0, (int)SDL_floorf(b[0] * scale));
        int y0 = SDL_max(0, (int)SDL_floorf(b[1] * scale));
        int x1 = SDL_min((int)SDL_ceilf(UI_WIDTH * scale), (int)SDL_ceilf(b[2] * scale));
        int y1 = SDL_min((int)SDL_ceilf(UI_HEIGHT * scale), (int)SDL_ceilf(b[3] * scale));
        int w = x1 - x0, h = y1 - y0;
        Uint8 *px = w > 0 && h > 0 ? SDL_calloc(w * h, 4) : NULL;
        if (px)
        {
            nsvgRasterize(rast, image, -x0, -y0, scale, px, w, h, w * 4);
            if (tint == SVG_BODY)
                add_grain(px, w, h);
            SDL_Texture *t = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_RGBA32,
                                               SDL_TEXTUREACCESS_STATIC, w, h);
            if (t)
            {
                SDL_UpdateTexture(t, NULL, px, w * 4);
                SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                svg_layers[svg_layer_count].texture = t;
                svg_layers[svg_layer_count].dest = (SDL_FRect)
                    { x0 / scale, y0 / scale, w / scale, h / scale };
                svg_layers[svg_layer_count].tint = tint;
                svg_layer_count++;
            }
            SDL_free(px);
        }
        run = end;
    }
out:
    if (rast)
        nsvgDeleteRasterizer(rast);
    if (image)
        nsvgDelete(image);
}

/* Draws the SVG device image and the LCD; false without an SVG skin */
static bool render_svg_skin(void)
{
    int out_w, out_h;
    if (!svg_skin || !background ||
        SDL_GetRendererOutputSize(sdlRenderer, &out_w, &out_h))
        return false;
    if (out_w != svg_layers_w || out_h != svg_layers_h)
        build_svg_layers(out_w, out_h);
    if (!svg_layer_count)
        return false;

    /* The colour picker sets the aluminium; the wheel follows it. Opacity
       0 (the original image, for the bitmap) is plain silver here. */
    SDL_Color body = { skin_tint.r, skin_tint.g, skin_tint.b, 255 };
    if (!skin_tint.opacity)
        body = (SDL_Color){ 0xc9, 0xcb, 0xcf, 255 };
    bool dark = luminance(body.r, body.g, body.b) < 80;
    SDL_Color wheel = dark ? (SDL_Color){ 0x2f, 0x2f, 0x32, 255 }
                           : (SDL_Color){ 0xf0, 0xf0, 0xf0, 255 };
    SDL_Color label = dark ? (SDL_Color){ 0x8e, 0x8e, 0x92, 255 }
                           : (SDL_Color){ 0x9a, 0x9a, 0x9a, 255 };

    SDL_RenderClear(sdlRenderer);
    for (int i = 0; i < svg_layer_count; i++)
    {
        SDL_Color c = { 255, 255, 255, 255 };
        if (svg_layers[i].tint == SVG_BODY)
            c = body;
        else if (svg_layers[i].tint == SVG_WHEEL)
            c = wheel;
        else if (svg_layers[i].tint == SVG_LABEL)
            c = label;
        SDL_SetTextureColorMod(svg_layers[i].texture, c.r, c.g, c.b);
        SDL_RenderCopyF(sdlRenderer, svg_layers[i].texture, NULL,
                        &svg_layers[i].dest);
    }
    SDL_Rect lcd = { UI_LCD_POSX, UI_LCD_POSY, SIM_LCD_WIDTH, SIM_LCD_HEIGHT };
    SDL_RenderCopy(sdlRenderer, gui_texture, &lcd, &lcd);
    SDL_RenderPresent(sdlRenderer);
    return true;
}

#ifdef __ANDROID__
#include <jni.h>

/* Called by the colour picker (SkinColourPicker.java), on Android's UI
   thread: shows the colour, and saves it if asked */
JNIEXPORT void JNICALL
Java_org_rockbox_sim_RockboxSimActivity_nativeSetSkinColour(JNIEnv *env,
        jclass cls, jint r, jint g, jint b, jint opacity, jboolean save)
{
    (void)env;
    (void)cls;
    if (!skin_image)
        return;

    SDL_LockMutex(window_mutex);
    skin_tint.r = SDL_clamp(r, 0, 255);
    skin_tint.g = SDL_clamp(g, 0, 255);
    skin_tint.b = SDL_clamp(b, 0, 255);
    skin_tint.opacity = SDL_clamp(opacity, 0, 100);
    if (!svg_skin) /* the SVG is coloured when rendering */
    {
        apply_skin_colour();
        sdl_window_adjustment_needed(true);
    }
    if (save)
        save_skin_colour();
    SDL_UnlockMutex(window_mutex);
    sdl_window_request_render();
}

static void show_skin_colour_picker(void)
{
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (!env || !activity)
        return;

    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID show = (*env)->GetMethodID(env, cls, "showSkinColourPicker",
                                         "(IIII)V");
    /* the SVG has no opacity: -1 hides it */
    if (show)
        (*env)->CallVoidMethod(env, activity, show, skin_tint.r, skin_tint.g,
                               skin_tint.b, svg_skin ? -1 : skin_tint.opacity);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}
#endif /* __ANDROID__ */

bool sdl_window_change_skin_colour(void)
{
    if (!skin_image)
        return false;
#ifdef __ANDROID__
    show_skin_colour_picker();
    return false; /* the picker redraws as the colour changes */
#else
    skin_preset = (skin_preset + 1) % SKIN_PRESETS;
    set_skin_preset();
    apply_skin_colour();
    save_skin_colour();
    return true;
#endif
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
/* The GL context behind the renderer can only be current on one thread, so
   only the event thread, which created it, renders. Other threads ask it
   to, and it then redraws the whole LCD from lcd_surface. */
static SDL_threadID render_thread;
static Uint32 render_event_type = (Uint32)-1;
static SDL_atomic_t render_requested;
static volatile bool window_hidden = false;

bool sdl_window_is_render_thread(void)
{
    return SDL_ThreadID() == render_thread;
}

void sdl_window_request_render(void)
{
    if (render_event_type == (Uint32)-1 ||
        !SDL_AtomicCAS(&render_requested, 0, 1))
        return; /* one pending is enough */

    SDL_Event ev;
    SDL_zero(ev);
    ev.type = render_event_type;
    /* SDL_RegisterEvents() may hand out SDL_USEREVENT, which power_off()
       also pushes: tell ours apart */
    ev.user.data1 = &render_event_type;
    if (SDL_PushEvent(&ev) <= 0)
        SDL_AtomicSet(&render_requested, 0);
}

bool sdl_window_handle_event(SDL_Event *event)
{
    if (event->type != render_event_type ||
        event->user.data1 != &render_event_type)
        return false;

    SDL_AtomicSet(&render_requested, 0);
    if (window_hidden)
        return true; /* no surface; redrawn when visible again */
    sdl_gui_update(lcd_surface, 0, 0, SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
                   SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
                   background ? UI_LCD_POSX : 0, background ? UI_LCD_POSY : 0);
    return true;
}

void sdl_window_set_visible(bool visible)
{
    window_hidden = !visible;
    if (visible)
        sdl_window_request_render();
}
#endif

void sdl_window_render(void)
{
#ifdef __ANDROID__
    if (window_hidden)
        return;
    if (!sdl_window_is_render_thread())
    {
        sdl_window_request_render();
        return;
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
#ifdef HAVE_SIM_TOUCHWHEEL
    if (!render_svg_skin())
#endif
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
#ifdef __ANDROID__
    if (!sdl_window_is_render_thread())
    {
        /* adjusted when the event thread renders */
        sdl_window_request_render();
        return true;
    }
#endif
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
        load_svg_skin();
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
#ifdef __ANDROID__
    render_thread = SDL_ThreadID();
    render_event_type = SDL_RegisterEvents(1);
#endif
}
