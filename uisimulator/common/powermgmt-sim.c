/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Heikki Hannikainen, Uwe Freese
 * Revisions copyright (C) 2005 by Gerald Van Baren
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
#include "config.h"
#include "system.h"
#include <time.h>
#include "kernel.h"
#include "powermgmt.h"
#include "power.h"
#ifdef __ANDROID__
#include <SDL.h>
#endif
#ifdef HAVE_SIM_3D
bool sim3d_cable(bool plugged); /* window-3d.c */
#endif

#define BATT_MINMVOLT   3300      /* minimum millivolts of battery */
#define BATT_MAXMVOLT   4300      /* maximum millivolts of battery */
#define BATT_MAXRUNTIME (10 * 60) /* maximum runtime with full battery in
                                     minutes */
/* Number of millivolts to discharge the battery every second */
#define BATT_DISCHARGE_STEP ((BATT_MAXMVOLT - BATT_MINMVOLT) / 100)
/* Number of millivolts to charge the battery every second */
#define BATT_CHARGE_STEP (BATT_DISCHARGE_STEP * 2)
#if CONFIG_CHARGING >= CHARGING_MONITOR
/* Number of seconds to externally power before discharging again */
#define POWER_AFTER_CHARGE_TICKS (8 * HZ)
#endif

static bool charging = false;
static unsigned int batt_millivolts = BATT_MAXMVOLT;
static unsigned int batt_percent = 100;
static unsigned int batt_runtime = BATT_MAXRUNTIME;
static unsigned int batt_current = 0;

void powermgmt_init_target(void) {}

static void battery_status_update(void)
{
    /* Delay next battery update until tick */
    static long update_after_tick = 0;
#if CONFIG_CHARGING >= CHARGING_MONITOR && !defined(__ANDROID__)
    /* When greater than 0, the tick to unplug the external power at */
    static unsigned int ext_power_until_tick = 0;
#endif

    if(TIME_BEFORE(current_tick, update_after_tick))
        return;

    update_after_tick = current_tick + HZ;

#ifdef __ANDROID__
    /* Report the phone's battery rather than a simulated one */
    int percent;
    SDL_PowerState state = SDL_GetPowerInfo(NULL, &percent);
    if (percent >= 0)
        batt_millivolts = BATT_MINMVOLT +
                          percent * (BATT_MAXMVOLT - BATT_MINMVOLT) / 100;
    charging = state == SDL_POWERSTATE_CHARGING;
#if CONFIG_CHARGING
    /* Only on a change: powermgmt broadcasts a charger event each time it
       sees a transitional state, and each one restarts the backlight's
       timeout */
    bool plugged = state == SDL_POWERSTATE_CHARGING ||
                   state == SDL_POWERSTATE_CHARGED;
#ifdef HAVE_SIM_3D
    /* only once the 3D device's dock cable is in (window-3d.c) */
    plugged = sim3d_cable(plugged);
    charging = charging && plugged;
#endif
    if (plugged && charger_input_state <= CHARGER_UNPLUGGED)
        charger_input_state = CHARGER_PLUGGED;
    else if (!plugged && charger_input_state >= CHARGER_PLUGGED)
        charger_input_state = CHARGER_UNPLUGGED;
#endif
#else /* !__ANDROID__ */
#if CONFIG_CHARGING >= CHARGING_MONITOR
    /* Handle period of being externally powered */
    if (ext_power_until_tick > 0) {
        if (TIME_AFTER(current_tick, ext_power_until_tick)) {
            /* Pretend the charger was disconnected */
            charger_input_state = CHARGER_UNPLUGGED;
            ext_power_until_tick = 0;
        }
        return;
    }
#endif

    if (charging) {
        batt_millivolts += BATT_CHARGE_STEP;
        if (batt_millivolts >= BATT_MAXMVOLT) {
            charging = false;
#if CONFIG_CHARGING >= CHARGING_MONITOR
            /* Keep external power until tick */
            ext_power_until_tick = current_tick + POWER_AFTER_CHARGE_TICKS;
#elif CONFIG_CHARGING
            /* Pretend the charger was disconnected */
            charger_input_state = CHARGER_UNPLUGGED;
#endif
        }
    } else {
        batt_millivolts -= BATT_DISCHARGE_STEP;
        if (batt_millivolts <= BATT_MINMVOLT) {
            charging = true;
#if CONFIG_CHARGING
            /* Pretend the charger was connected */
            charger_input_state = CHARGER_PLUGGED;
#endif
        }
    }
#endif /* __ANDROID__ */

    batt_percent = ((float) (batt_millivolts - BATT_MINMVOLT) / (BATT_MAXMVOLT - BATT_MINMVOLT)) * 100;
    batt_runtime = batt_percent * BATT_MAXRUNTIME;
    /* current is completely bogus... */
    batt_current = charging ? BATT_CHARGE_STEP : BATT_DISCHARGE_STEP;
}

unsigned short battery_level_disksafe = 3200;
unsigned short battery_level_shutoff = 3200;

/* make the simulated curve nicely linear */
unsigned short percent_to_volt_discharge[11] =
{ 3300, 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200, 4300 };
unsigned short percent_to_volt_charge[11] =
{ 3300, 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200, 4300  };

#if CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE
int _battery_voltage(void)
{
    battery_status_update();
    return batt_millivolts;
}
#endif

#if CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE
int _battery_level(void)
{
    battery_status_update();
    return batt_percent;
}
#endif

#if (CONFIG_BATTERY_MEASURE & TIME_MEASURE)
int _battery_time(void)
{
    battery_status_update();
    return batt_runtime;
}
#endif

#if (CONFIG_BATTERY_MEASURE & CURRENT_MEASURE)
int _battery_current(void)
{
    battery_status_update();
    return batt_current;
}
#endif

#if CONFIG_CHARGING
unsigned int power_input_status(void)
{
    unsigned int status = charger_input_state >= CHARGER_PLUGGED
            ? POWER_INPUT_CHARGER : POWER_INPUT_NONE;

#ifdef HAVE_BATTERY_SWITCH
    status |= POWER_INPUT_BATTERY;
#endif

    return status;
}

bool charging_state(void)
{
    return charging;
}
#endif

#ifdef HAVE_ACCESSORY_SUPPLY
void accessory_supply_set(bool enable)
{
    (void)enable;
}
#endif

#ifdef HAVE_LINEOUT_POWEROFF
void lineout_set(bool enable)
{
    (void)enable;
}
#endif

#ifdef HAVE_REMOTE_LCD
bool remote_detect(void)
{
    return true;
}
#endif

#ifdef HAVE_BATTERY_SWITCH
unsigned int input_millivolts(void)
{
    if ((power_input_status() & POWER_INPUT_BATTERY) == 0) {
        /* Just return a safe value if battery isn't connected */
        return 4050;
    }

    return battery_voltage();
}
#endif
