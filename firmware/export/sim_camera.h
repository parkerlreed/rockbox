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

/* The phone's camera, for the simulator running on Android
   (HAVE_SIM_CAMERA, see target/hosted/sdl/camera-sdl.c) */

#ifndef __SIM_CAMERA_H__
#define __SIM_CAMERA_H__

#include <stdbool.h>

/* the same as in SimCamera.java */
enum {
    SIM_CAMERA_OFF,
    SIM_CAMERA_WAITING, /* for permission, or the first frame */
    SIM_CAMERA_ON,
    SIM_CAMERA_DENIED,  /* no permission */
    SIM_CAMERA_FAILED,  /* no such camera, or it went away */
};

/* Opens the front or the rear camera, asking for permission if needed */
int sim_camera_open(bool front);
void sim_camera_close(void);
int sim_camera_state(void);

/* The latest frame, upright and cropped to fill width x height: brightness
   0-255, or with colour R, G and B for each pixel; false before the first */
bool sim_camera_frame(unsigned char *dst, int width, int height, bool colour);

/* Tells the phone's gallery about a photo saved at path (in the simdisk) */
void sim_camera_saved(const char *path);

#endif /* __SIM_CAMERA_H__ */
