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

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

/* Picks the colour of the device body and centre button by hue, saturation
 * and brightness, previewing it on the device image as the sliders move */
class SkinColourPicker
{
    private static final String[] NAMES =
        { "Hue", "Saturation", "Brightness", "Opacity" };
    private static final int[] MAX = { 360, 100, 100, 100 };
    private static final int HUE = 0, SAT = 1, VAL = 2, OPACITY = 3;
    private static final int DEFAULT_OPACITY = 80;

    /* hasOpacity false: the device image (SVG) has no opacity to set, so
     * there is no slider; Cancel still restores the opacity as it was */
    static void show(Activity activity, int r, int g, int b, int opacity,
                     final boolean hasOpacity)
    {
        final int[] old = { r, g, b, opacity };
        float[] hsv = new float[3];
        Color.RGBToHSV(r, g, b, hsv);
        /* starting from the original image, start with some colour */
        final int[] value = { Math.round(hsv[0]), Math.round(hsv[1] * 100),
                              Math.round(hsv[2] * 100),
                              !hasOpacity ? 100
                              : opacity > 0 ? opacity : DEFAULT_OPACITY };
        float density = activity.getResources().getDisplayMetrics().density;
        final int pad = (int)(20 * density);
        final int track = (int)(10 * density);

        LinearLayout layout = new LinearLayout(activity);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad / 2, pad, 0);

        final View swatch = new View(activity);
        layout.addView(swatch, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, (int)(32 * density)));

        final SeekBar[] bars = new SeekBar[NAMES.length];
        final Runnable update = () -> {
            swatch.setBackgroundColor(colour(value));
            for (int i = HUE; i <= VAL; i++)
                bars[i].setProgressDrawable(gradient(i, value, track));
        };

        for (int i = 0; i < (hasOpacity ? NAMES.length : OPACITY); i++)
        {
            final int c = i;
            final TextView label = new TextView(activity);
            label.setText(label(c, value[c]));
            label.setPadding(0, pad / 2, 0, 0);
            layout.addView(label);

            SeekBar bar = new SeekBar(activity);
            bars[c] = bar;
            bar.setMax(MAX[c]);
            bar.setProgress(value[c]);
            bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener()
            {
                @Override
                public void onProgressChanged(SeekBar s, int progress, boolean user)
                {
                    if (!user)
                        return;
                    value[c] = progress;
                    label.setText(label(c, progress));
                    update.run();
                    set(value, false);
                }
                @Override public void onStartTrackingTouch(SeekBar s) {}
                @Override public void onStopTrackingTouch(SeekBar s) {}
            });
            layout.addView(bar);
        }
        update.run();

        AlertDialog dialog = new AlertDialog.Builder(activity,
                android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("iPod colour")
            .setView(layout)
            .setPositiveButton("Save", (d, w) -> set(value, true))
            .setNegativeButton("Cancel", (d, w) -> send(old, false))
            .setNeutralButton("Reset", (d, w) -> {
                int c = colour(value);
                send(new int[] { Color.red(c), Color.green(c), Color.blue(c), 0 },
                     true);
            })
            .setOnCancelListener(d -> send(old, false))
            .create();

        /* keep the device image visible and undimmed while previewing */
        Window window = dialog.getWindow();
        if (window != null)
        {
            window.setGravity(Gravity.BOTTOM);
            window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        }
        dialog.show();
        set(value, false);
    }

    private static int colour(int[] v)
    {
        return Color.HSVToColor(new float[] { v[HUE], v[SAT] / 100f, v[VAL] / 100f });
    }

    /* Track showing what moving slider c would do */
    private static GradientDrawable gradient(int c, int[] v, int height)
    {
        int[] colours;
        if (c == HUE)
        {
            colours = new int[7];
            for (int i = 0; i < 7; i++)
                colours[i] = Color.HSVToColor(new float[] { i * 60, 1, 1 });
        }
        else
        {
            int[] lo = v.clone(), hi = v.clone();
            lo[c] = 0;
            hi[c] = 100;
            colours = new int[] { colour(lo), colour(hi) };
        }
        GradientDrawable d = new GradientDrawable(
            GradientDrawable.Orientation.LEFT_RIGHT, colours);
        d.setCornerRadius(height / 2f);
        d.setSize(0, height);
        return d;
    }

    private static String label(int c, int v)
    {
        return NAMES[c] + ": " + v + (c == HUE ? "°" : "%");
    }

    private static void set(int[] v, boolean save)
    {
        int c = colour(v);
        send(new int[] { Color.red(c), Color.green(c), Color.blue(c), v[OPACITY] },
             save);
    }

    private static void send(int[] rgba, boolean save)
    {
        RockboxSimActivity.nativeSetSkinColour(rgba[0], rgba[1], rgba[2], rgba[3],
                                               save);
    }
}
