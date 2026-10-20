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

package org.rockbox.sim;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import org.libsdl.app.SDLActivity;

/* Runs the simulator (libmain.so) with shared storage (/sdcard) as its
 * disk, and its own files in /sdcard/Rockbox/.rockbox (see filesystem-sim.c).
 * RockboxSimLauncher gets storage access first. */
public class RockboxSimActivity extends SDLActivity
{
    private static final String TAG = "RockboxSim";
    private static Vibrator vibrator;
    private File root;
    private File rockboxHome;

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        root = Environment.getExternalStorageDirectory();
        rockboxHome = new File(root, "Rockbox");
        try
        {
            unpackFiles();
        }
        catch (IOException e)
        {
            Log.e(TAG, "unpacking the simdisk failed", e);
        }
        super.onCreate(savedInstanceState);
        setUpVibrator();
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(
                Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {
                Manifest.permission.POST_NOTIFICATIONS }, 0);
        startForegroundService(new Intent(this, RockboxSimService.class));
    }

    @Override
    protected void onDestroy()
    {
        stopService(new Intent(this, RockboxSimService.class));
        super.onDestroy();
    }

    private void setUpVibrator()
    {
        vibrator = Build.VERSION.SDK_INT >= 31
            ? getSystemService(VibratorManager.class).getDefaultVibrator()
            : (Vibrator)getSystemService(VIBRATOR_SERVICE);
        if (vibrator != null && !vibrator.hasVibrator())
            vibrator = null;
    }

    /* Called from the simulator (button-sdl.c) as the clickwheel turns
     * (kind 0) or clicks (kind 1); strength 1 to 3. Short pulses at a set
     * amplitude are firmer than the predefined effects on many phones. */
    static void haptic(int kind, int strength)
    {
        if (vibrator == null)
            return;
        int ms = kind == 1 ? 20 : 10;
        int amplitude = vibrator.hasAmplitudeControl()
            ? Math.round(255 * (0.25f + strength * 0.25f)) /* weak half, strong full */
            : VibrationEffect.DEFAULT_AMPLITUDE;
        vibrator.vibrate(VibrationEffect.createOneShot(ms, amplitude));
    }

    /* Colour of the device body and centre button, see window-sdl.c */
    static native void nativeSetSkinColour(int r, int g, int b, int opacity,
                                           boolean save);

    /* Called from the simulator when the hidden colour spot is tapped */
    public void showSkinColourPicker(final int r, final int g, final int b,
                                     final int opacity)
    {
        runOnUiThread(() -> SkinColourPicker.show(this, r, g, b, opacity));
    }

    @Override
    protected String[] getArguments()
    {
        return new String[] { "--root", root.getPath() };
    }

    /* Unpack assets/files.zip once per install or update, or when
     * .rockbox was deleted: simdisk/ goes to /sdcard/Rockbox, the rest
     * (device image) to the private files dir. Files the simulator
     * created (settings, database) are left alone. */
    private void unpackFiles() throws IOException
    {
        File dir = getFilesDir();
        File stamp = new File(dir, "files.stamp");
        String version;
        try
        {
            version = Long.toString(getPackageManager()
                .getPackageInfo(getPackageName(), 0).lastUpdateTime);
        }
        catch (PackageManager.NameNotFoundException e)
        {
            throw new IOException(e);
        }
        boolean installed = new File(rockboxHome, ".rockbox/rockbox-info.txt").exists();
        if (installed && stamp.exists() && version.equals(readFile(stamp)))
            return;

        byte[] buf = new byte[65536];
        try (ZipInputStream zip = new ZipInputStream(getAssets().open("files.zip")))
        {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null)
            {
                String name = entry.getName();
                File base = dir;
                if (name.startsWith("simdisk/"))
                {
                    base = rockboxHome;
                    name = name.substring("simdisk/".length());
                }
                File out = new File(base, name);
                if (!out.getCanonicalPath().startsWith(base.getCanonicalPath() + File.separator))
                {
                    if (name.isEmpty())
                        continue;
                    throw new IOException("bad zip entry " + entry.getName());
                }
                if (entry.isDirectory())
                {
                    out.mkdirs();
                    continue;
                }
                out.getParentFile().mkdirs();
                try (OutputStream os = new FileOutputStream(out))
                {
                    int n;
                    while ((n = zip.read(buf)) > 0)
                        os.write(buf, 0, n);
                }
            }
        }
        try (OutputStream os = new FileOutputStream(stamp))
        {
            os.write(version.getBytes());
        }
    }

    private static String readFile(File f) throws IOException
    {
        try (InputStream is = new java.io.FileInputStream(f))
        {
            byte[] buf = new byte[64];
            int n = is.read(buf);
            return n > 0 ? new String(buf, 0, n) : "";
        }
    }
}
