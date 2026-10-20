/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Daniel Stenberg
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
#define RB_FILESYSTEM_OS
#include <stdio.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "load_code.h"
#include "rbpaths.h"
#include "debug.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#include <string.h>

/* Android won't map code from app data as executable, so codecs and
 * plugins ship in the APK next to libmain_<model>.so, named after the
 * model and their path, as the APK holds several models:
 * /.rockbox/rocks/games/foo.rock -> libipodvideo_rocks_games_foo_rock.so
 * (see uisimulator/android/build-apk.sh) */
static int android_lib_path(char *buf, size_t bufsize, const char *filename)
{
    Dl_info info;
    const char *rel = strstr(filename, ROCKBOX_DIR "/");
    if (!rel || !dladdr((void *)lc_open, &info))
        return -1;
    rel += sizeof(ROCKBOX_DIR "/") - 1;

    int dirlen = strrchr(info.dli_fname, '/') - info.dli_fname;
    int n = snprintf(buf, bufsize, "%.*s/lib" TARGET_NAME "_%s",
                     dirlen, info.dli_fname, rel);
    if (n < 0 || (size_t)n + 3 >= bufsize)
        return -1;
    for (char *p = buf + dirlen + 1; *p; p++)
        if (*p == '/' || *p == '.')
            *p = '_';
    strcat(buf, ".so");
    return 0;
}
#endif

void * lc_open(const char *filename, unsigned char *buf, size_t buf_size)
{
    char osfilename[SIM_TMPBUF_MAX_PATH];
#ifdef __ANDROID__
    if (android_lib_path(osfilename, sizeof (osfilename), filename) < 0)
        return NULL;
#else
    if (sim_get_os_path(osfilename, filename, sizeof (osfilename)) < 0)
        return NULL;
#endif

    return os_lc_open(osfilename);
    (void)buf; (void)buf_size;
}
