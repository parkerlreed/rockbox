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
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.widget.Toast;

import java.io.File;

/* Gets access to shared storage, the simulator's disk, before starting
 * the simulator. Runs in a process of its own, so that it can also start
 * the simulator again once the previous one has quit, when switching
 * models (see RockboxSimActivity.switchModel()). */
public class RockboxSimLauncher extends Activity
{
    static final String EXTRA_RESTART_AFTER = "restart_after_pid";
    private static final int RESTART_WAIT_MS = 10000;

    private boolean asked = false;
    private int waitForPid;
    private long waitUntil;
    private final Handler handler = new Handler(Looper.getMainLooper());

    static boolean hasStorageAccess(Activity activity)
    {
        if (Build.VERSION.SDK_INT >= 30)
            return Environment.isExternalStorageManager();
        return activity.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
            == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        waitForPid = getIntent().getIntExtra(EXTRA_RESTART_AFTER, 0);
        waitUntil = System.currentTimeMillis() + RESTART_WAIT_MS;
    }

    @Override
    protected void onResume()
    {
        super.onResume();
        /* the simulator of the previous model must be gone first, as a
           process can only hold one */
        if (waitForPid != 0 && new File("/proc/" + waitForPid).exists() &&
            System.currentTimeMillis() < waitUntil)
        {
            handler.postDelayed(this::onResume, 100);
            return;
        }
        waitForPid = 0;
        if (hasStorageAccess(this))
        {
            startActivity(new Intent(this, RockboxSimActivity.class));
            finish();
        }
        else if (!asked)
        {
            asked = true;
            if (Build.VERSION.SDK_INT >= 30)
                startActivity(new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            else
                requestPermissions(new String[] {
                    Manifest.permission.WRITE_EXTERNAL_STORAGE }, 0);
        }
        else
        {
            Toast.makeText(this, "Rockbox needs access to all files to use "
                + "the phone's storage as its disk", Toast.LENGTH_LONG).show();
            finish();
        }
    }
}
