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
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Size;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.Collections;

/* The phone's camera for the camera plugin (camera-sdl.c): each frame goes
 * to nativeFrame() */
class SimCamera
{
    private static final String TAG = "RockboxSim";
    static final int PERMISSION_REQUEST = 0x0ca;

    /* the same as in sim_camera.h */
    static final int OFF = 0, WAITING = 1, ON = 2, DENIED = 3, FAILED = 4;

    static native void nativeFrame(ByteBuffer y, int yRowStride,
                                   ByteBuffer u, ByteBuffer v,
                                   int uvRowStride, int uvPixelStride,
                                   int width, int height, int orientation,
                                   boolean front);
    static native void nativeState(int state);

    private static HandlerThread thread;
    private static Handler handler;
    /* only touched on the camera thread */
    private static CameraDevice device;
    private static CameraCaptureSession session;
    private static ImageReader reader;
    /* callbacks from an earlier start are ignored */
    private static int generation;
    private static boolean wantFront;
    private static boolean waitingForPermission;

    static synchronized void start(Activity activity, boolean front)
    {
        if (thread == null)
        {
            thread = new HandlerThread("SimCamera");
            thread.start();
            handler = new Handler(thread.getLooper());
        }
        wantFront = front;
        if (activity.checkSelfPermission(Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED)
        {
            waitingForPermission = true;
            activity.runOnUiThread(() -> activity.requestPermissions(
                new String[] { Manifest.permission.CAMERA }, PERMISSION_REQUEST));
            return;
        }
        final int gen = ++generation;
        handler.post(() -> open(activity, front, gen));
    }

    static synchronized void stop()
    {
        waitingForPermission = false;
        if (handler == null)
            return;
        generation++;
        handler.post(SimCamera::close);
    }

    static synchronized void permissionResult(Activity activity, boolean granted)
    {
        if (!waitingForPermission)
            return;
        waitingForPermission = false;
        if (granted)
            start(activity, wantFront);
        else
            nativeState(DENIED);
    }

    /* camera thread */
    private static void close()
    {
        if (session != null)
            session.close();
        if (device != null)
            device.close();
        if (reader != null)
            reader.close();
        session = null;
        device = null;
        reader = null;
    }

    private static boolean current(int gen)
    {
        synchronized (SimCamera.class)
        {
            return gen == generation;
        }
    }

    private static void fail(int gen, String why, Exception e)
    {
        Log.e(TAG, "camera: " + why, e);
        if (!current(gen))
            return;
        close();
        nativeState(FAILED);
    }

    /* camera thread */
    private static void open(Activity activity, boolean front, int gen)
    {
        close();
        if (!current(gen))
            return;
        CameraManager manager =
            (CameraManager)activity.getSystemService(Context.CAMERA_SERVICE);
        try
        {
            String id = null;
            CameraCharacteristics chars = null;
            int facing = front ? CameraCharacteristics.LENS_FACING_FRONT
                               : CameraCharacteristics.LENS_FACING_BACK;
            for (String i : manager.getCameraIdList())
            {
                CameraCharacteristics c = manager.getCameraCharacteristics(i);
                Integer f = c.get(CameraCharacteristics.LENS_FACING);
                if (f != null && f == facing)
                {
                    id = i;
                    chars = c;
                    break;
                }
            }
            if (id == null)
            {
                fail(gen, "no " + (front ? "front" : "rear") + " camera", null);
                return;
            }

            Size size = smallSize(chars);
            Integer o = chars.get(CameraCharacteristics.SENSOR_ORIENTATION);
            final int sensor = o != null ? o : 90;
            reader = ImageReader.newInstance(size.getWidth(), size.getHeight(),
                                             ImageFormat.YUV_420_888, 2);
            reader.setOnImageAvailableListener(r -> {
                Image image = r.acquireLatestImage();
                if (image == null)
                    return;
                if (current(gen))
                {
                    Image.Plane[] p = image.getPlanes();
                    nativeFrame(p[0].getBuffer(), p[0].getRowStride(),
                                p[1].getBuffer(), p[2].getBuffer(),
                                p[1].getRowStride(), p[1].getPixelStride(),
                                image.getWidth(), image.getHeight(),
                                orientation(activity, sensor, front), front);
                }
                image.close();
            }, handler);

            manager.openCamera(id, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera)
                {
                    if (!current(gen))
                    {
                        camera.close();
                        return;
                    }
                    device = camera;
                    startSession(gen);
                }

                @Override
                public void onDisconnected(CameraDevice camera)
                {
                    camera.close();
                    if (camera == device)
                        fail(gen, "disconnected", null);
                }

                @Override
                public void onError(CameraDevice camera, int error)
                {
                    camera.close();
                    if (camera == device || device == null)
                        fail(gen, "error " + error, null);
                }
            }, handler);
        }
        catch (CameraAccessException | SecurityException | IllegalArgumentException e)
        {
            fail(gen, "opening failed", e);
        }
    }

    /* camera thread */
    private static void startSession(int gen)
    {
        try
        {
            final Surface surface = reader.getSurface();
            device.createCaptureSession(Collections.singletonList(surface),
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession s)
                    {
                        if (!current(gen) || device == null)
                        {
                            s.close();
                            return;
                        }
                        session = s;
                        try
                        {
                            CaptureRequest.Builder b = device.createCaptureRequest(
                                CameraDevice.TEMPLATE_PREVIEW);
                            b.addTarget(surface);
                            s.setRepeatingRequest(b.build(), null, handler);
                        }
                        catch (CameraAccessException | IllegalStateException e)
                        {
                            fail(gen, "preview failed", e);
                        }
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession s)
                    {
                        fail(gen, "configuring failed", null);
                    }
                }, handler);
        }
        catch (CameraAccessException | IllegalStateException e)
        {
            fail(gen, "session failed", e);
        }
    }

    /* The smallest at least 320x240, the biggest screen */
    private static Size smallSize(CameraCharacteristics chars)
    {
        StreamConfigurationMap map =
            chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size best = null;
        for (Size s : map.getOutputSizes(ImageFormat.YUV_420_888))
        {
            if (s.getWidth() < 320 || s.getHeight() < 240)
                continue;
            if (best == null || (long)s.getWidth() * s.getHeight() <
                                (long)best.getWidth() * best.getHeight())
                best = s;
        }
        return best != null ? best : new Size(640, 480);
    }

    /* Degrees clockwise to turn a frame upright on the screen as it is
       held now; the front camera's is mirrored afterwards */
    @SuppressWarnings("deprecation")
    private static int orientation(Activity activity, int sensor, boolean front)
    {
        int degrees;
        switch (activity.getWindowManager().getDefaultDisplay().getRotation())
        {
            case Surface.ROTATION_90:  degrees = 90;  break;
            case Surface.ROTATION_180: degrees = 180; break;
            case Surface.ROTATION_270: degrees = 270; break;
            default:                   degrees = 0;   break;
        }
        return front ? (sensor + degrees) % 360
                     : (sensor - degrees + 360) % 360;
    }
}
