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

/* The phone's camera for the simulator on Android: SimCamera.java runs it
   and hands over each frame as YUV 4:2:0 */

#include <SDL.h>
#include <jni.h>
#include "config.h"
#include "sim_camera.h"
#include "fs_defines.h"
#include "filesystem-sim.h"

static SDL_mutex *frame_lock;
/* the latest frame: brightness, then the half size U and V planes */
static unsigned char *frame;
static size_t frame_size;
static int frame_w, frame_h;
static int frame_orientation;   /* degrees clockwise to turn it upright */
static bool frame_front;        /* shown mirrored */
static volatile int camera_state = SIM_CAMERA_OFF;

/* Calls a static method of RockboxSimActivity */
static void call_activity(const char *name, const char *sig, jboolean arg)
{
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (!env || !activity)
        return;

    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = (*env)->GetStaticMethodID(env, cls, name, sig);
    if (method)
        (*env)->CallStaticVoidMethod(env, cls, method, arg);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

int sim_camera_open(bool front)
{
    if (!frame_lock)
        frame_lock = SDL_CreateMutex();
    SDL_LockMutex(frame_lock);
    frame_w = frame_h = 0; /* no frame from the other camera */
    SDL_UnlockMutex(frame_lock);

    camera_state = SIM_CAMERA_WAITING;
    call_activity("cameraStart", "(Z)V", front);
    return camera_state;
}

void sim_camera_close(void)
{
    call_activity("cameraStop", "(Z)V", JNI_FALSE);
    if (!frame_lock)
        return;
    SDL_LockMutex(frame_lock);
    camera_state = SIM_CAMERA_OFF;
    frame_w = frame_h = 0;
    SDL_UnlockMutex(frame_lock);
}

int sim_camera_state(void)
{
    return camera_state;
}

static inline unsigned char clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

bool sim_camera_frame(unsigned char *dst, int width, int height, bool colour)
{
    if (!frame_lock)
        return false;
    SDL_LockMutex(frame_lock);
    if (!frame_w)
    {
        SDL_UnlockMutex(frame_lock);
        return false;
    }

    int sw = frame_w, sh = frame_h, o = frame_orientation;
    int cw = (sw + 1) / 2, ch = (sh + 1) / 2;
    const unsigned char *u = &frame[sw * sh], *v = &u[cw * ch];
    bool sideways = o == 90 || o == 270;
    int uw = sideways ? sh : sw, uh = sideways ? sw : sh; /* upright */

    /* scale to fill, cropping what doesn't fit */
    float scale = SDL_max((float)width / uw, (float)height / uh);
    float ox = (uw - width / scale) / 2, oy = (uh - height / scale) / 2;

    for (int y = 0; y < height; y++)
    {
        int uy = SDL_clamp((int)(oy + (y + 0.5f) / scale), 0, uh - 1);
        for (int x = 0; x < width; x++)
        {
            int ux = SDL_clamp((int)(ox + (x + 0.5f) / scale), 0, uw - 1);
            if (frame_front)
                ux = uw - 1 - ux;

            int sx, sy;
            switch (o)
            {
            case 90:  sx = uy;          sy = sh - 1 - ux; break;
            case 180: sx = sw - 1 - ux; sy = sh - 1 - uy; break;
            case 270: sx = sw - 1 - uy; sy = ux;          break;
            default:  sx = ux;          sy = uy;          break;
            }
            int luma = frame[sy * sw + sx];
            if (!colour)
            {
                *dst++ = luma;
                continue;
            }

            /* BT.601, full range, in 1/1024ths */
            int c = (sy / 2) * cw + sx / 2;
            int cb = u[c] - 128, cr = v[c] - 128;
            *dst++ = clamp255(luma + ((1436 * cr) >> 10));
            *dst++ = clamp255(luma - ((352 * cb + 731 * cr) >> 10));
            *dst++ = clamp255(luma + ((1815 * cb) >> 10));
        }
    }
    SDL_UnlockMutex(frame_lock);
    return true;
}

void sim_camera_saved(const char *path)
{
    char os_path[MAX_PATH];
    if (sim_get_os_path(os_path, path, sizeof os_path) < 0)
        return;

    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (!env || !activity)
        return;

    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = (*env)->GetStaticMethodID(env, cls, "mediaScan",
                                                 "(Ljava/lang/String;)V");
    jstring jpath = (*env)->NewStringUTF(env, os_path);
    if (method && jpath)
        (*env)->CallStaticVoidMethod(env, cls, method, jpath);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (jpath)
        (*env)->DeleteLocalRef(env, jpath);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/* Copies a plane, which may have padding at the end of rows and between
   pixels, without reading past its end */
static void copy_plane(unsigned char *dst, const unsigned char *src,
                       jlong size, int width, int height, int row_stride,
                       int pixel_stride)
{
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            jlong i = (jlong)y * row_stride + (jlong)x * pixel_stride;
            *dst++ = i < size ? src[i] : 128;
        }
    }
}

/* From SimCamera.java, on its camera thread: the planes of a YUV_420_888
   frame */
JNIEXPORT void JNICALL
Java_org_rockbox_sim_SimCamera_nativeFrame(JNIEnv *env, jclass cls,
        jobject y_plane, jint y_row_stride,
        jobject u_plane, jobject v_plane, jint uv_row_stride,
        jint uv_pixel_stride, jint width, jint height,
        jint orientation, jboolean front)
{
    (void)cls;
    const unsigned char *ys = (*env)->GetDirectBufferAddress(env, y_plane);
    const unsigned char *us = (*env)->GetDirectBufferAddress(env, u_plane);
    const unsigned char *vs = (*env)->GetDirectBufferAddress(env, v_plane);
    if (!ys || !us || !vs || !frame_lock)
        return;

    SDL_LockMutex(frame_lock);
    if (camera_state == SIM_CAMERA_OFF)
    {
        SDL_UnlockMutex(frame_lock);
        return;
    }
    int cw = (width + 1) / 2, ch = (height + 1) / 2;
    size_t size = (size_t)width * height + 2 * (size_t)cw * ch;
    if (size > frame_size)
    {
        unsigned char *p = SDL_realloc(frame, size);
        if (!p)
        {
            SDL_UnlockMutex(frame_lock);
            return;
        }
        frame = p;
        frame_size = size;
    }
    copy_plane(frame, ys, (*env)->GetDirectBufferCapacity(env, y_plane),
               width, height, y_row_stride, 1);
    unsigned char *u = &frame[width * height];
    copy_plane(u, us, (*env)->GetDirectBufferCapacity(env, u_plane),
               cw, ch, uv_row_stride, uv_pixel_stride);
    copy_plane(&u[cw * ch], vs, (*env)->GetDirectBufferCapacity(env, v_plane),
               cw, ch, uv_row_stride, uv_pixel_stride);
    frame_w = width;
    frame_h = height;
    frame_orientation = orientation;
    frame_front = front;
    camera_state = SIM_CAMERA_ON;
    SDL_UnlockMutex(frame_lock);
}

JNIEXPORT void JNICALL
Java_org_rockbox_sim_SimCamera_nativeState(JNIEnv *env, jclass cls,
                                           jint state)
{
    (void)env;
    (void)cls;
    camera_state = state;
}
