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

/* The device as a 3D model, drawn with OpenGL ES 3 on Android: the model
   and its textures come from uisimulator/skins/ipodmini2g-3d.py, the
   textures are decoded by RockboxSimActivity.decodeImage(). The LCD is
   shown on the display part, a tap on the face goes to the 2D skin's
   touch handling (button-sdl.c) at the matching point, and dragging off
   the wheel turns the device. */

#include <SDL.h>
#include <GLES3/gl3.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <jni.h>
#include <math.h>
#include <stdio.h>
#include "config.h"
#include "sim-ui-defines.h"
#include "buttonmap.h"
#include "window-3d.h"
#include "window-sdl.h"

#define FOVY            (28.f * (float)M_PI / 180)
#define START_PITCH     0.22f   /* tilted to show the top, with hold */
#define HOLD_TOUCH_PAD  3.f     /* mm round the hold switch that take taps */

enum role { ROLE_PART, ROLE_BODY, ROLE_DISPLAY, ROLE_GLASS, ROLE_SWITCH,
            ROLE_WHEEL, ROLE_CABLE };

struct material
{
    char name[32];
    float base[4], metallic, roughness;
    char tex_name[3][32];       /* colour, occlusion/roughness/metal, normal */
    GLuint tex[3];
};

struct part
{
    char name[32];
    unsigned material, role, nverts, nidx;
    GLuint vao, vbo, ibo;
};

/* from model.bin, in mm, in the order ipodmini2g-3d.py writes them */
static struct
{
    float half[3];              /* half the device's size */
    float wheel[2], wheel_r, button_r, front_z;
    float lcd[4];               /* x0, y0, x1, y1 */
    float switch_min[3], switch_max[3], travel;
    float orange[5];            /* the switch is orange left of and below
                                   these x, y, unmoved; 3 unused */
    float body_avg[3];
} meta;

static struct material *materials;
static unsigned nmaterials;
static struct part *parts;
static unsigned nparts;

static SDL_Window *window;
static SDL_GLContext context;
static GLuint program, bg_program, bg_vao, lcd_tex;
static struct
{
    GLint mvp, rot, offset, role, base, metallic, roughness, has, camera,
          tint, tint_mix, tint_ref, lcd_rect, lcd_size, lcd_bg,
          tex_base, tex_orm, tex_normal, lcd, orange;
} u;

/* the device's turn, as a quaternion w, x, y, z in the view's axes: x to
   the right, y up, z towards the viewer. It starts tilted to show the top. */
static float turn[4] = { 1, 0, 0, 0 };
static bool resetting;
/* the reset: a short animation from where it was, which ends whatever the
   gyroscope does meanwhile */
#define RESET_SECONDS 0.35f
static float reset_t, reset_turn[4], reset_zoom, reset_pan[2];
static volatile bool gyro_wanted;
static int screen_orientation = SDL_ORIENTATION_PORTRAIT; /* as last drawn */
static SDL_SpinLock turn_lock;  /* the gyroscope's thread turns it too */
static float zoom = 1, pan[2];  /* pan in mm, in the view's plane */
static float switch_pos, switch_target;

/* The dock cable slides in from below when the phone charges, and out
   when it stops. Where it is goes by the time, so it gets there even when
   nothing is drawn, with the app in the background. */
#define CABLE_SECONDS   0.7f
#define CABLE_TRAVEL    70.f    /* mm below, out of sight */
static bool have_cable;
static SDL_SpinLock cable_lock;
static bool cable_in;
static float cable_from;
static Uint32 cable_t0;

/* 0 out, 1 plugged in */
static float cable_at(Uint32 now)
{
    /* a frame can start just before the battery thread starts the cable */
    float d = SDL_max((Sint32)(now - cable_t0), 0) / (CABLE_SECONDS * 1000);
    return cable_in ? SDL_min(cable_from + d, 1) : SDL_max(cable_from - d, 0);
}
static Uint32 last_ms;
static float aspect = 0.5f, distance = 300;
static float last_mvp[16];      /* as last drawn, for sim3d_hold_slide() */

/* Column-major 4x4 matrices */
static void mat_mul(float *out, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 4; i++)
            r[c * 4 + i] = a[i] * b[c * 4] + a[4 + i] * b[c * 4 + 1] +
                           a[8 + i] * b[c * 4 + 2] + a[12 + i] * b[c * 4 + 3];
    SDL_memcpy(out, r, sizeof(r));
}

/* Quaternions, w, x, y, z */
static void quat_mul(float out[4], const float a[4], const float b[4])
{
    float r[4] = { a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
                   a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
                   a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
                   a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0] };
    float l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    for (int i = 0; i < 4; i++)
        out[i] = r[i] / l;
}

/* Turns the device by angle about an axis of the view, a unit vector */
static void turn_by(float x, float y, float z, float angle)
{
    float s = sinf(angle / 2);
    float q[4] = { cosf(angle / 2), x * s, y * s, z * s };
    SDL_AtomicLock(&turn_lock);
    quat_mul(turn, q, turn);
    SDL_AtomicUnlock(&turn_lock);
}

static void start_turn(float q[4])
{
    q[0] = cosf(START_PITCH / 2);
    q[1] = sinf(START_PITCH / 2);
    q[2] = q[3] = 0;
}

/* the device's turn, as a 3x3 matrix, rows first */
static void rotation(float r[9])
{
    SDL_AtomicLock(&turn_lock);
    float w = turn[0], x = turn[1], y = turn[2], z = turn[3];
    SDL_AtomicUnlock(&turn_lock);
    float m[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                   2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                   2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) };
    SDL_memcpy(r, m, sizeof(m));
}

static const char *vertex_src =
    "#version 300 es\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat3 u_rot;\n"
    "uniform vec3 u_offset;\n"
    "out vec3 v_world;\n"
    "out vec3 v_model;\n"
    "out vec3 v_normal;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec3 p = a_pos + u_offset;\n"
    "    v_model = p;\n"
    "    v_world = u_rot * p;\n"
    "    v_normal = u_rot * a_normal;\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_mvp * vec4(p, 1.0);\n"
    "}\n";

/* Metal and plastic lit by a studio: a bright softbox above left, a strip
   light to the right and a grey room, which the aluminium reflects */
static const char *fragment_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec3 v_world;\n"
    "in vec3 v_model;\n"
    "in vec3 v_normal;\n"
    "in vec2 v_uv;\n"
    "uniform int u_role;\n"
    "uniform vec4 u_base;\n"
    "uniform float u_metallic, u_roughness;\n"
    "uniform ivec3 u_has;\n"
    "uniform sampler2D u_tex_base, u_tex_orm, u_tex_normal, u_lcd;\n"
    "uniform vec3 u_camera;\n"
    "uniform vec3 u_tint, u_tint_ref;\n"
    "uniform float u_tint_mix;\n"
    "uniform vec4 u_lcd_rect;\n"
    "uniform vec2 u_lcd_size;\n"
    "uniform vec3 u_lcd_bg;\n"
    "uniform vec3 u_offset;\n"
    "uniform vec2 u_orange;\n"
    "out vec4 frag;\n"
    "const float PI = 3.14159265;\n"
    "const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);\n"
    "vec3 linear(vec3 c) { return pow(c, vec3(2.2)); }\n"
    "vec3 display(vec3 x) {\n"
    "    x = x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14);\n"
    "    return pow(clamp(x, 0.0, 1.0), vec3(1.0 / 2.2));\n"
    "}\n"
    /* tangent frame from derivatives, as the model has no tangents */
    "mat3 tangent_frame(vec3 n, vec3 p, vec2 uv) {\n"
    "    vec3 dp1 = dFdx(p), dp2 = dFdy(p);\n"
    "    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);\n"
    "    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);\n"
    "    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;\n"
    "    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;\n"
    "    float s = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-20));\n"
    "    return mat3(t * s, b * s, n);\n"
    "}\n"
    "vec3 studio(vec3 r, float rough) {\n"
    "    vec3 c = mix(vec3(0.03, 0.03, 0.035), vec3(0.42, 0.43, 0.46),\n"
    "                 smoothstep(-0.4, 0.9, r.y));\n"
    "    float spread = 0.05 + rough * 0.8;\n"
    "    float edge = mix(0.985, 0.55, rough);\n"
    "    float box = smoothstep(edge - spread, edge,\n"
    "                           dot(r, normalize(vec3(-0.45, 0.75, 0.55))));\n"
    "    float strip = smoothstep(edge - spread, edge,\n"
    "                             dot(r, normalize(vec3(0.85, 0.15, 0.35))));\n"
    "    float fade = 1.0 - rough * 0.7;\n"
    "    return c + (box * 1.4 + strip * 0.45) * fade;\n"
    "}\n"
    "float ggx(float noh, float a) {\n"
    "    float a2 = a * a, d = noh * noh * (a2 - 1.0) + 1.0;\n"
    "    return a2 / (PI * d * d);\n"
    "}\n"
    "void main() {\n"
    "    vec3 n = normalize(v_normal);\n"
    "    if (!gl_FrontFacing) n = -n;\n"
    "    if (u_role == 2) {\n"
    /* the LCD, crisp at any size, its background round it */
    "        vec2 uv = (v_model.xy - u_lcd_rect.xy) / (u_lcd_rect.zw - u_lcd_rect.xy);\n"
    "        uv.y = 1.0 - uv.y;\n"
    "        vec3 c = u_lcd_bg;\n"
    "        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {\n"
    "            vec2 t = uv * u_lcd_size, seam = floor(t + 0.5);\n"
    "            t = seam + clamp((t - seam) / max(fwidth(t), vec2(1e-4)), -0.5, 0.5);\n"
    "            c = texture(u_lcd, t / u_lcd_size).rgb;\n"
    "        }\n"
    "        frag = vec4(c * (0.9 + 0.1 * max(n.z, 0.0)), 1.0);\n"
    "        return;\n"
    "    }\n"
    "    if (u_role == 4 && v_model.x - u_offset.x < u_orange.x && v_model.y < u_orange.y) {\n"
    /* hold's orange, painted on the switch's plate, barely lit */
    "        frag = vec4(vec3(0.95, 0.42, 0.08) * (0.8 + 0.2 * max(n.y, 0.0)), 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec3 v = normalize(u_camera - v_world);\n"
    "    vec3 base = u_base.rgb;\n"
    "    float ao = 1.0, rough = u_roughness, metal = u_metallic;\n"
    "    if (u_has.x != 0) {\n"
    "        vec3 t = texture(u_tex_base, v_uv).rgb;\n"
    "        base *= linear(t);\n"
    /* the wheel's labels, grey on its light grey, printed in the case's
       colour as on the mini 2G */
    "        if (u_role == 5) {\n"
    "            vec3 ink = u_tint_mix > 0.0 ? u_tint : u_tint_ref;\n"
    "            base = mix(base, ink, smoothstep(0.74, 0.62, dot(t, vec3(1.0 / 3.0))));\n"
    "        }\n"
    "    }\n"
    "    if (u_has.y != 0) {\n"
    "        vec3 orm = texture(u_tex_orm, v_uv).rgb;\n"
    "        ao = orm.r; rough *= orm.g; metal *= orm.b;\n"
    "    }\n"
    "    if (u_has.z != 0) {\n"
    "        vec3 t = texture(u_tex_normal, v_uv).xyz * 2.0 - 1.0;\n"
    "        t.y = -t.y;\n"
    "        n = normalize(tangent_frame(n, v_world, v_uv) * t);\n"
    "    }\n"
    /* anodised aluminium: satin, its colour in the oxide over the metal */
    "    if (u_role == 1) { rough = max(rough, 0.6); metal *= 0.5; }\n"
    "    if (u_role == 1 && u_tint_mix > 0.0) {\n"
    /* the anodising, not the grey and white printing on the back */
    "        float hi = max(base.r, max(base.g, base.b));\n"
    "        float sat = (hi - min(base.r, min(base.g, base.b))) / max(hi, 1e-4);\n"
    "        float l = dot(base, LUMA) / max(dot(u_tint_ref, LUMA), 1e-3);\n"
    "        base = mix(base, u_tint * l, u_tint_mix * smoothstep(0.25, 0.6, sat));\n"
    "    }\n"

    "    rough = clamp(rough, 0.04, 1.0);\n"
    "    float a = rough * rough;\n"
    "    float nov = max(dot(n, v), 1e-3);\n"
    "    vec3 f0 = mix(vec3(0.04), base, metal);\n"
    "    vec3 f = f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(1.0 - nov, 5.0);\n"
    "    vec3 r = reflect(-v, n);\n"
    "    if (u_role == 3) {\n"
    /* the glass: only reflections, more at a glancing angle */
    "        float fg = 0.04 + 0.96 * pow(1.0 - nov, 5.0);\n"
    "        frag = vec4(display(studio(r, 0.02) * fg), fg);\n"
    "        return;\n"
    "    }\n"
    "    vec3 diffuse = base * (1.0 - metal);\n"
    "    vec3 room = mix(vec3(0.10, 0.10, 0.11), vec3(0.50, 0.51, 0.53), n.y * 0.5 + 0.5)\n"
    "                + 0.18 * max(n.z, 0.0);\n"
    "    vec3 c = (diffuse * room * (1.0 - f) + studio(r, rough) * f) * ao;\n"
    "    vec3 dirs[2] = vec3[2](normalize(vec3(-0.4, 0.8, 0.6)), normalize(vec3(0.7, 0.2, 0.5)));\n"
    "    vec3 cols[2] = vec3[2](vec3(2.4, 2.35, 2.25), vec3(0.6, 0.65, 0.72));\n"
    "    for (int i = 0; i < 2; i++) {\n"
    "        vec3 l = dirs[i], h = normalize(l + v);\n"
    "        float nol = max(dot(n, l), 0.0);\n"
    "        float k = a * 0.5;\n"
    "        float vis = 0.25 / ((nov * (1.0 - k) + k) * (nol * (1.0 - k) + k));\n"
    "        vec3 fl = f0 + (1.0 - f0) * pow(1.0 - max(dot(h, v), 0.0), 5.0);\n"
    "        c += (diffuse * (1.0 - fl) / PI + ggx(max(dot(n, h), 0.0), a) * vis * fl)\n"
    "             * cols[i] * nol * ao;\n"
    "    }\n"
    "    frag = vec4(display(c), 1.0);\n"
    "}\n";

/* The backdrop: dark grey, lighter towards the top */
static const char *bg_vertex_src =
    "#version 300 es\n"
    "out vec2 v_pos;\n"
    "void main() {\n"
    "    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "    v_pos = p;\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *bg_fragment_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_pos;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    float g = mix(0.07, 0.20, v_pos.y * 0.5 + 0.5);\n"
    "    g *= 1.0 - 0.25 * dot(v_pos * 0.6, v_pos * 0.6);\n"
    "    frag = vec4(vec3(g, g * 1.02, g * 1.06), 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("3D: shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link(const char *vs, const char *fs)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    GLint ok = 0;
    if (!v || !f)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        printf("3D: program: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* Decodes an image with Android's decoder into a mipmapped texture */
static GLuint load_texture(const char *dir, const char *name)
{
    char path[512];
    GLuint tex = 0;
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (!name[0] || !env || !activity)
        return 0;

    SDL_snprintf(path, sizeof(path), "%s/%s", dir, name);
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID decode = (*env)->GetStaticMethodID(env, cls, "decodeImage",
                            "(Ljava/lang/String;[I)Ljava/nio/ByteBuffer;");
    jstring jpath = (*env)->NewStringUTF(env, path);
    jintArray jsize = (*env)->NewIntArray(env, 2);
    jobject pixels = decode && jpath && jsize ?
        (*env)->CallStaticObjectMethod(env, cls, decode, jpath, jsize) : NULL;
    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionClear(env);
        pixels = NULL;
    }
    if (pixels)
    {
        jint size[2];
        (*env)->GetIntArrayRegion(env, jsize, 0, 2, size);
        void *data = (*env)->GetDirectBufferAddress(env, pixels);
        if (data)
        {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size[0], size[1], 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, data);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            /* GL_EXT_texture_filter_anisotropic, on nearly every phone */
            glTexParameterf(GL_TEXTURE_2D, 0x84FE, 8.f);
            glGetError();
        }
        (*env)->DeleteLocalRef(env, pixels);
    }
    else
        printf("3D: can't decode %s\n", path);
    if (jpath)
        (*env)->DeleteLocalRef(env, jpath);
    if (jsize)
        (*env)->DeleteLocalRef(env, jsize);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return tex;
}

static void upload_part(struct part *p, const float *verts, const Uint32 *idx)
{
    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, p->nverts * 8 * sizeof(float), verts,
                 GL_STATIC_DRAW);
    glGenBuffers(1, &p->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, p->nidx * sizeof(Uint32), idx,
                 GL_STATIC_DRAW);
    for (int i = 0; i < 3; i++)
        glEnableVertexAttribArray(i);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
    glBindVertexArray(0);
}

/* Reads model.bin into GL buffers; false if it's missing or wrong */
static bool load_model(const char *dir)
{
    char path[512];
    size_t size;
    SDL_snprintf(path, sizeof(path), "%s/model.bin", dir);
    Uint8 *data = SDL_LoadFile(path, &size), *p = data, *end = data + size;
    if (!data)
        return false;

#define TAKE(dst, n) do { if ((size_t)(end - p) < (size_t)(n)) goto bad; \
                          SDL_memcpy((dst), p, (n)); p += (n); } while (0)
    char magic[4];
    Uint32 version, count;
    TAKE(magic, 4);
    TAKE(&version, 4);
    if (SDL_memcmp(magic, "RB3D", 4) || version != 1)
        goto bad;
    TAKE(&count, 4);
    if (count != sizeof(meta) / sizeof(float))
        goto bad;
    TAKE(&meta, sizeof(meta));

    TAKE(&nmaterials, 4);
    materials = SDL_calloc(nmaterials, sizeof(*materials));
    for (unsigned i = 0; i < nmaterials; i++)
    {
        struct material *m = &materials[i];
        TAKE(m->name, 32);
        TAKE(m->base, 4 * sizeof(float));
        TAKE(&m->metallic, sizeof(float));
        TAKE(&m->roughness, sizeof(float));
        TAKE(m->tex_name, sizeof(m->tex_name));
        for (int t = 0; t < 3; t++)
            m->tex_name[t][31] = '\0';
    }

    TAKE(&nparts, 4);
    parts = SDL_calloc(nparts, sizeof(*parts));
    for (unsigned i = 0; i < nparts; i++)
    {
        struct part *pt = &parts[i];
        TAKE(pt->name, 32);
        TAKE(&pt->material, 4);
        TAKE(&pt->role, 4);
        TAKE(&pt->nverts, 4);
        TAKE(&pt->nidx, 4);
        size_t vbytes = (size_t)pt->nverts * 8 * sizeof(float);
        size_t ibytes = (size_t)pt->nidx * sizeof(Uint32);
        if (pt->material >= nmaterials ||
            (size_t)(end - p) < vbytes + ibytes)
            goto bad;
        for (Uint32 k = 0; k < pt->nidx; k++)
        {
            Uint32 v;
            SDL_memcpy(&v, p + vbytes + k * 4, 4);
            if (v >= pt->nverts)
                goto bad;
        }
        upload_part(pt, (const float *)p, (const Uint32 *)(p + vbytes));
        if (pt->role == ROLE_CABLE)
            have_cable = true;
        p += vbytes + ibytes;
    }
#undef TAKE
    SDL_free(data);

    for (unsigned i = 0; i < nmaterials; i++)
        for (int t = 0; t < 3; t++)
            materials[i].tex[t] = load_texture(dir, materials[i].tex_name[t]);
    return true;

bad:
    printf("3D: %s is damaged\n", path);
    SDL_free(data);
    return false;
}

static void lookup_uniforms(void)
{
#define U(name) u.name = glGetUniformLocation(program, "u_" #name)
    U(mvp); U(rot); U(offset); U(role); U(base); U(metallic); U(roughness);
    U(has); U(camera); U(tint); U(tint_mix); U(tint_ref); U(lcd_rect);
    U(lcd_size); U(lcd_bg); U(tex_base); U(tex_orm); U(tex_normal); U(lcd);
    U(orange);
#undef U
}

SDL_Window *sim3d_create_window(const char *dir, const char *title,
                                int w, int h, Uint32 flags)
{
    for (int samples = 4; samples >= 0; samples -= 4)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, samples ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, samples);
        window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, w, h,
                                  flags | SDL_WINDOW_OPENGL);
        if (window && (context = SDL_GL_CreateContext(window)))
            break;
        if (window)
            SDL_DestroyWindow(window);
        window = NULL;
    }
    if (!window)
    {
        printf("3D: no GL ES 3 context: %s\n", SDL_GetError());
        return NULL;
    }
    SDL_GL_SetSwapInterval(1);
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    printf("3D: GL %s, %d samples\n", glGetString(GL_VERSION), samples);

    program = link(vertex_src, fragment_src);
    bg_program = link(bg_vertex_src, bg_fragment_src);
    if (!program || !bg_program || !load_model(dir))
    {
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        context = NULL;
        window = NULL;
        return NULL;
    }
    lookup_uniforms();
    glGenVertexArrays(1, &bg_vao);

    glGenTextures(1, &lcd_tex);
    glBindTexture(GL_TEXTURE_2D, lcd_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, SIM_LCD_WIDTH, SIM_LCD_HEIGHT, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    last_ms = SDL_GetTicks();
    start_turn(turn);
    return window;
}

bool sim3d_active(void)
{
    return context != NULL;
}

static void draw_part(const struct part *p, float offset_x, float offset_y)
{
    const struct material *m = &materials[p->material];
    glUniform1i(u.role, p->role);
    glUniform3f(u.offset, offset_x, offset_y, 0);
    glUniform4fv(u.base, 1, m->base);
    glUniform1f(u.metallic, m->metallic);
    glUniform1f(u.roughness, m->roughness);
    glUniform3i(u.has, m->tex[0] != 0, m->tex[1] != 0, m->tex[2] != 0);
    for (int t = 0; t < 3; t++)
    {
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, m->tex[t]);
    }
    glBindVertexArray(p->vao);
    glDrawElements(GL_TRIANGLES, p->nidx, GL_UNSIGNED_INT, 0);
}

static float approach(float v, float target, float rate, float dt)
{
    float k = 1 - expf(-rate * dt);
    v += (target - v) * k;
    return fabsf(target - v) < 1e-4f ? target : v;
}

bool sim3d_render(SDL_Surface *lcd, const struct sim3d_look *look)
{
    int w, h;
    if (!context)
        return false;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (w <= 0 || h <= 0)
        return false;

    screen_orientation = SDL_GetDisplayOrientation(SDL_GetWindowDisplayIndex(window));

    Uint32 now = SDL_GetTicks();
    float dt = SDL_min((now - last_ms) / 1000.f, 0.1f);
    last_ms = now;
    if (resetting)
    {
        /* back the short way round, easing in and out */
        float q[4], d = 0;
        reset_t = SDL_min(reset_t + dt / RESET_SECONDS, 1);
        float s = reset_t * reset_t * (3 - 2 * reset_t);
        start_turn(q);
        for (int i = 0; i < 4; i++)
            d += reset_turn[i] * q[i];
        SDL_AtomicLock(&turn_lock);
        for (int i = 0; i < 4; i++)
            turn[i] = reset_turn[i] + ((d < 0 ? -q[i] : q[i]) - reset_turn[i]) * s;
        quat_mul(turn, (float[4]){ 1, 0, 0, 0 }, turn); /* normalises */
        SDL_AtomicUnlock(&turn_lock);
        zoom = reset_zoom + (1 - reset_zoom) * s;
        pan[0] = reset_pan[0] * (1 - s);
        pan[1] = reset_pan[1] * (1 - s);
        resetting = reset_t < 1;
    }
    switch_pos = approach(switch_pos, switch_target, 25, dt);

    /* the device fills the window, whichever way round it is */
    aspect = (float)w / h;
    float t = tanf(FOVY / 2);
    distance = (SDL_max(meta.half[1] * 1.12f / t,
                        meta.half[0] * 1.12f / (t * aspect)) + meta.half[2]) / zoom;

    float n = 10, f = distance + 200;
    float proj[16] = { 1 / (t * aspect), 0, 0, 0,
                       0, 1 / t, 0, 0,
                       0, 0, -(f + n) / (f - n), -1,
                       0, 0, -2 * f * n / (f - n), 0 };
    float r[9];
    rotation(r);
    float model[16] = { r[0], r[3], r[6], 0,
                        r[1], r[4], r[7], 0,
                        r[2], r[5], r[8], 0,
                        pan[0], pan[1], -distance, 1 };
    float mvp[16];
    mat_mul(mvp, proj, model);
    SDL_memcpy(last_mvp, mvp, sizeof(mvp));
    float rot3[9] = { r[0], r[3], r[6], r[1], r[4], r[7], r[2], r[5], r[8] };

    if (lcd)
    {
        glBindTexture(GL_TEXTURE_2D, lcd_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, lcd->pitch / 2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
                        GL_RGB, GL_UNSIGNED_SHORT_5_6_5, lcd->pixels);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(bg_program);
    glBindVertexArray(bg_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(program);
    glUniformMatrix4fv(u.mvp, 1, GL_FALSE, mvp);
    glUniformMatrix3fv(u.rot, 1, GL_FALSE, rot3);
    glUniform3f(u.camera, -pan[0], -pan[1], distance);
    glUniform1i(u.tex_base, 0);
    glUniform1i(u.tex_orm, 1);
    glUniform1i(u.tex_normal, 2);
    glUniform1i(u.lcd, 3);
    glUniform3f(u.tint, powf(look->body.r / 255.f, 2.2f),
                powf(look->body.g / 255.f, 2.2f), powf(look->body.b / 255.f, 2.2f));
    glUniform1f(u.tint_mix, look->tinted ? 1.f : 0.f);
    glUniform3f(u.tint_ref, powf(meta.body_avg[0], 2.2f),
                powf(meta.body_avg[1], 2.2f), powf(meta.body_avg[2], 2.2f));
    glUniform4fv(u.lcd_rect, 1, meta.lcd);
    glUniform2f(u.lcd_size, SIM_LCD_WIDTH, SIM_LCD_HEIGHT);
    /* only on hold: off, a sliver of the plate shows by the cap's end */
    glUniform2f(u.orange, switch_pos > meta.travel / 2 ? meta.orange[0] : -1e9f,
                meta.orange[1]);
    glUniform3f(u.lcd_bg, look->lcd_bg.r / 255.f, look->lcd_bg.g / 255.f,
                look->lcd_bg.b / 255.f);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, lcd_tex);

    SDL_AtomicLock(&cable_lock);
    float cable = cable_at(now);
    SDL_AtomicUnlock(&cable_lock);
    /* it slows down as it goes in */
    float cable_y = -CABLE_TRAVEL * (1 - cable) * (1 - cable) * (1 - cable);
    for (unsigned i = 0; i < nparts; i++)
        if (parts[i].role == ROLE_CABLE)
        {
            if (cable > 0)
                draw_part(&parts[i], 0, cable_y);
        }
        else if (parts[i].role != ROLE_GLASS)
            draw_part(&parts[i], parts[i].role == ROLE_SWITCH ? switch_pos : 0, 0);

    /* the glass last, over what's behind it */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (unsigned i = 0; i < nparts; i++)
        if (parts[i].role == ROLE_GLASS)
            draw_part(&parts[i], 0, 0);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);

    SDL_GL_SwapWindow(window);
    return resetting || switch_pos != switch_target ||
           (cable > 0 && cable < 1) || (cable_in && cable < 1);
}

/* The ray through a point in the window, in the model's coordinates */
static void ray(int x, int y, float o[3], float d[3])
{
    int w, h;
    float r[9];
    SDL_GetWindowSize(window, &w, &h);
    float t = tanf(FOVY / 2);
    float v[3] = { (2.f * x / w - 1) * t * aspect, (1 - 2.f * y / h) * t, -1 };
    float c[3] = { -pan[0], -pan[1], distance };
    rotation(r);
    /* by the transpose, the inverse turn */
    for (int i = 0; i < 3; i++)
    {
        o[i] = r[i] * c[0] + r[3 + i] * c[1] + r[6 + i] * c[2];
        d[i] = r[i] * v[0] + r[3 + i] * v[1] + r[6 + i] * v[2];
    }
}

/* Where the ray meets a box, or -1 */
static float hit_box(const float o[3], const float d[3],
                     const float lo[3], const float hi[3])
{
    float t0 = 0, t1 = 1e9f;
    for (int i = 0; i < 3; i++)
    {
        if (fabsf(d[i]) < 1e-9f)
        {
            if (o[i] < lo[i] || o[i] > hi[i])
                return -1;
            continue;
        }
        float a = (lo[i] - o[i]) / d[i], b = (hi[i] - o[i]) / d[i];
        t0 = SDL_max(t0, SDL_min(a, b));
        t1 = SDL_min(t1, SDL_max(a, b));
    }
    return t0 <= t1 ? t0 : -1;
}

static void to_ui(float x, float y, int *ui_x, int *ui_y)
{
    float s = tw.outer_radius / meta.wheel_r;
    *ui_x = lroundf(tw.x + (x - meta.wheel[0]) * s);
    *ui_y = lroundf(tw.y - (y - meta.wheel[1]) * s);
}

enum sim3d_hit sim3d_pick(int x, int y, int *ui_x, int *ui_y)
{
    float o[3], d[3];
    if (!context)
        return SIM3D_NONE;
    ray(x, y, o, d);

    float lo[3], hi[3];
    for (int i = 0; i < 3; i++)
    {
        lo[i] = meta.switch_min[i] - HOLD_TOUCH_PAD;
        hi[i] = meta.switch_max[i] + HOLD_TOUCH_PAD;
    }
    lo[0] += switch_pos;
    hi[0] += switch_pos;
    lo[1] = meta.half[1] - HOLD_TOUCH_PAD; /* only above the face's top */
    if (hit_box(o, d, lo, hi) >= 0)
        return SIM3D_HOLD;

    if (d[2] < 0)
    {
        float t = (meta.front_z - o[2]) / d[2];
        float px = o[0] + d[0] * t, py = o[1] + d[1] * t;
        if (t > 0 && fabsf(px) < meta.half[0] && fabsf(py) < meta.half[1])
        {
            to_ui(px, py, ui_x, ui_y);
            return SIM3D_FACE;
        }
    }
    return SIM3D_NONE;
}

void sim3d_face_point(int x, int y, int *ui_x, int *ui_y)
{
    float o[3], d[3];
    ray(x, y, o, d);
    if (fabsf(d[2]) < 1e-6f)
        d[2] = -1e-6f;
    float t = (meta.front_z - o[2]) / d[2];
    to_ui(o[0] + d[0] * t, o[1] + d[1] * t, ui_x, ui_y);
}

void sim3d_turn(int dx, int dy)
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    float k = (float)M_PI / SDL_max(SDL_min(w, h), 1);
    resetting = false;
    turn_by(0, 1, 0, dx * k);
    turn_by(1, 0, 0, dy * k);
}

/* The gyroscope, read with Android's sensor API at its full rate, which
   SDL's sensors don't allow, on a thread of its own. Each reading turns
   the device back by as much as the phone turned. */
static void gyro_turn(const float rate[3], float dt)
{
    /* the phone's axes, as it's held upright, to the screen's */
    float x = rate[0], y = rate[1], z = rate[2];
    switch (screen_orientation)
    {
    case SDL_ORIENTATION_LANDSCAPE:         /* its right side up */
        x = -rate[1]; y = rate[0];
        break;
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED: /* its left side up */
        x = rate[1]; y = -rate[0];
        break;
    case SDL_ORIENTATION_PORTRAIT_FLIPPED:
        x = -rate[0]; y = -rate[1];
        break;
    default:
        break;
    }
    float r = sqrtf(x * x + y * y + z * z);
    if (r * dt < 1e-6f)
        return;
    /* the phone turned by this; the device turns back, staying put */
    turn_by(x / r, y / r, z / r, -r * dt);
}

static int gyro_thread_main(void *unused)
{
    (void)unused;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    ASensorManager *sm = ASensorManager_getInstance();
#pragma GCC diagnostic pop
    const ASensor *sensor = sm ?
        ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_GYROSCOPE) : NULL;
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorEventQueue *queue = sensor ?
        ASensorManager_createEventQueue(sm, looper, 1, NULL, NULL) : NULL;
    if (!queue)
    {
        printf("3D: no gyroscope\n");
        gyro_wanted = false;
        return 0;
    }
    ASensorEventQueue_enableSensor(queue, sensor);
    /* as fast as it goes, more than 200 a second with the manifest's
       HIGH_SAMPLING_RATE_SENSORS */
    ASensorEventQueue_setEventRate(queue, sensor, ASensor_getMinDelay(sensor));
    printf("3D: gyroscope every %d us\n", ASensor_getMinDelay(sensor));

    int64_t last = 0;
    while (gyro_wanted)
    {
        ASensorEvent ev[32];
        ssize_t n;
        ALooper_pollOnce(100, NULL, NULL, NULL);
        while ((n = ASensorEventQueue_getEvents(queue, ev, 32)) > 0)
        {
            for (ssize_t e = 0; e < n; e++)
            {
                if (ev[e].type != ASENSOR_TYPE_GYROSCOPE)
                    continue;
                float dt = (ev[e].timestamp - last) / 1e9f;
                if (last && dt > 0 && dt < 0.1f)
                    gyro_turn(ev[e].data, dt);
                last = ev[e].timestamp;
            }
            sdl_window_request_render();
        }
    }
    ASensorEventQueue_disableSensor(queue, sensor);
    ASensorManager_destroyEventQueue(sm, queue);
    return 0;
}

void sim_gyro_set(bool on)
{
    static SDL_Thread *thread;
    if (on && !thread)
    {
        gyro_wanted = true;
        thread = SDL_CreateThread(gyro_thread_main, "gyroscope", NULL);
    }
    else if (!on && thread)
    {
        gyro_wanted = false;
        SDL_WaitThread(thread, NULL);
        thread = NULL;
    }
}

/* mm in the view's plane per pixel in the window, at the device */
static float mm_per_pixel(void)
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    return 2 * distance * tanf(FOVY / 2) / SDL_max(h, 1);
}

void sim3d_zoom(float factor, int x, int y)
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    float before = mm_per_pixel();
    float z = SDL_clamp(zoom * factor, 0.6f, 8.f);
    resetting = false;
    /* keep the point under the fingers where it is */
    float after = before * zoom / z;
    pan[0] += (x - w / 2.f) * (after - before);
    pan[1] -= (y - h / 2.f) * (after - before);
    zoom = z;
    distance *= before ? after / before : 1;
}

void sim3d_pan(int dx, int dy)
{
    float k = mm_per_pixel();
    resetting = false;
    pan[0] += dx * k;
    pan[1] -= dy * k;
}

void sim3d_reset_view(void)
{
    SDL_AtomicLock(&turn_lock);
    SDL_memcpy(reset_turn, turn, sizeof(turn));
    SDL_AtomicUnlock(&turn_lock);
    reset_zoom = zoom;
    reset_pan[0] = pan[0];
    reset_pan[1] = pan[1];
    reset_t = 0;
    resetting = true;
}

/* Where a point of the model is in the window */
static void to_window(const float p[3], float *x, float *y)
{
    int w, h;
    float c[4];
    SDL_GetWindowSize(window, &w, &h);
    for (int i = 0; i < 4; i++)
        c[i] = last_mvp[i] * p[0] + last_mvp[4 + i] * p[1] +
               last_mvp[8 + i] * p[2] + last_mvp[12 + i];
    if (c[3] < 1e-6f)
        c[3] = 1e-6f;
    *x = (c[0] / c[3] * 0.5f + 0.5f) * w;
    *y = (0.5f - c[1] / c[3] * 0.5f) * h;
}

float sim3d_hold_slide(int x0, int y0, int x, int y)
{
    float p[3], q[3], px, py, qx, qy;
    for (int i = 0; i < 3; i++)
        q[i] = p[i] = (meta.switch_min[i] + meta.switch_max[i]) / 2;
    p[0] += switch_pos;
    q[0] = p[0] + 1; /* a mm towards hold */
    to_window(p, &px, &py);
    to_window(q, &qx, &qy);
    float sx = qx - px, sy = qy - py, s2 = sx * sx + sy * sy;
    return s2 > 1e-6f ? ((x - x0) * sx + (y - y0) * sy) / s2 : 0;
}

float sim3d_hold_travel(void)
{
    return meta.travel;
}

bool sim3d_cable(bool plugged)
{
    if (!context || !have_cable)
        return plugged;
    Uint32 now = SDL_GetTicks();
    SDL_AtomicLock(&cable_lock);
    if (plugged != cable_in)
    {
        cable_from = cable_at(now);
        cable_t0 = now;
        cable_in = plugged;
    }
    bool there = cable_at(now) >= 1;
    SDL_AtomicUnlock(&cable_lock);
    sdl_window_request_render();
    return plugged && there;
}

void sim3d_set_hold(bool on)
{
    switch_target = on ? meta.travel : 0;
}
