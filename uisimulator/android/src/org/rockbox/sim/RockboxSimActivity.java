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
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import org.libsdl.app.SDLActivity;

/* Runs the simulator of the chosen model (libmain_<model>.so) with shared
 * storage (/sdcard) as its disk, its own files in
 * /sdcard/Rockbox/<model>/.rockbox (see filesystem-sim.c), and its device
 * image and colour in files/<model>. RockboxSimLauncher gets storage
 * access first. */
public class RockboxSimActivity extends SDLActivity
{
    private static final String TAG = "RockboxSim";
    private static final String PREF_MODEL = "model";
    private static Vibrator vibrator;
    private File root;
    private File rockboxHome;
    /* the models in the APK (assets/models.txt): names and titles */
    private final List<String> models = new ArrayList<>();
    private final List<String> titles = new ArrayList<>();
    private String model;

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        root = Environment.getExternalStorageDirectory();
        rockboxHome = new File(root, "Rockbox");
        readModels();
        model = prefs().getString(PREF_MODEL, models.get(0));
        if (!models.contains(model))
            model = models.get(0);
        moveOldFiles();
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
                                     final int opacity, final boolean hasOpacity)
    {
        runOnUiThread(() ->
            SkinColourPicker.show(this, r, g, b, opacity, hasOpacity));
    }

    @Override
    protected String[] getLibraries()
    {
        return new String[] { "SDL2", "main_" + model };
    }

    @Override
    protected String[] getArguments()
    {
        /* built with SIM_DEBUG_BUTTONS=1 (build-apk.sh) */
        try
        {
            getAssets().open("debug_buttons").close();
            return new String[] { "--root", root.getPath(), "--debugbuttons" };
        }
        catch (IOException e)
        {
            return new String[] { "--root", root.getPath() };
        }
    }

    private SharedPreferences prefs()
    {
        return getSharedPreferences("rockbox", MODE_PRIVATE);
    }

    private void readModels()
    {
        try (BufferedReader r = new BufferedReader(
                new InputStreamReader(getAssets().open("models.txt"))))
        {
            String line;
            while ((line = r.readLine()) != null)
            {
                String[] f = line.split("\t", 2);
                if (f.length == 2)
                {
                    models.add(f[0]);
                    titles.add(f[1]);
                }
            }
        }
        catch (IOException e)
        {
            Log.e(TAG, "reading the models failed", e);
        }
        if (models.isEmpty())
        {
            models.add("ipodvideo");
            titles.add("iPod Video");
        }
    }

    /* Before there were several models, the iPod Video kept its files in
       /sdcard/Rockbox/.rockbox and files/ */
    private void moveOldFiles()
    {
        File old = new File(rockboxHome, ".rockbox");
        File video = new File(rockboxHome, "ipodvideo");
        if (old.isDirectory() && !video.exists() && video.mkdirs())
            old.renameTo(new File(video, ".rockbox"));
        File colour = new File(getFilesDir(), "skincolour.txt");
        File videoFiles = new File(getFilesDir(), "ipodvideo");
        videoFiles.mkdirs();
        if (colour.exists())
            colour.renameTo(new File(videoFiles, "skincolour.txt"));
        /* each model has its own now */
        for (String f : new String[] { "UI256.bmp", "skin.svg", "stdout.txt" })
            new File(getFilesDir(), f).delete();
    }

    /* Called from the simulator, Settings > Android > Model: the models
     * in the APK as "name\ttitle\n" lines */
    public String modelList()
    {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < models.size(); i++)
            sb.append(models.get(i)).append('\t').append(titles.get(i)).append('\n');
        return sb.toString();
    }

    /* A process can only run one model: shut this one down like holding
       Play, and have the launcher, in a process of its own, start the
       chosen one once it is gone */
    public void switchModel(String next)
    {
        if (!models.contains(next))
            return;
        prefs().edit().putString(PREF_MODEL, next).commit();
        startActivity(new Intent(this, RockboxSimLauncher.class)
            .putExtra(RockboxSimLauncher.EXTRA_RESTART_AFTER,
                      android.os.Process.myPid())
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        nativeShutDown();
    }

    static native void nativeShutDown();

    /* Unpack assets/files.zip once per install or update, or when this
     * model's .rockbox was deleted: models/<model>/simdisk/ goes to
     * /sdcard/Rockbox/<model>, the rest of models/<model>/ (device image) to
     * files/<model>. Files the simulator created (settings, database) are
     * left alone. */
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
        boolean installed = new File(rockboxHome,
            model + "/.rockbox/rockbox-info.txt").exists();
        if (installed && stamp.exists() && version.equals(readFile(stamp)))
            return;

        byte[] buf = new byte[65536];
        try (ZipInputStream zip = new ZipInputStream(getAssets().open("files.zip")))
        {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null)
            {
                /* models/<model>/simdisk/... or models/<model>/... */
                String[] parts = entry.getName().split("/", 3);
                if (parts.length < 3 || !parts[0].equals("models") ||
                    !parts[1].matches("[a-z0-9]+"))
                    continue;
                String name = parts[2];
                File base = new File(dir, parts[1]);
                if (name.startsWith("simdisk/") || name.equals("simdisk"))
                {
                    base = new File(rockboxHome, parts[1]);
                    name = name.substring(Math.min(name.length(), "simdisk/".length()));
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
