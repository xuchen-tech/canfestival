/*
This file is part of CanFestival, a library implementing CanOpen Stack.

Copyright (C): Edouard TISSERANT and Francis DUPIN


/* Weak no-op stubs for CAN port registration helpers.
 * Drivers in the lib may reference these symbols; the application may
 * provide real implementations (strong symbols). Marking them weak here
 * avoids unresolved symbol errors when linking the library. */
void RegisterCanPortForCOData(void* d, void* port) __attribute__((weak));
void UnregisterCanPortForCOData(void* d, void* port) __attribute__((weak));

void RegisterCanPortForCOData(void* d, void* port)
{
    (void)d; (void)port;
}

void UnregisterCanPortForCOData(void* d, void* port)
{
    (void)d; (void)port;
}
/*
See COPYING file for copyrights details.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
/*!
** @file   timer.c
** @author Edouard TISSERANT and Francis DUPIN
** @date   Tue Jun  5 09:32:32 2007
**
** @brief
**
**
*/

/* #define DEBUG_WAR_CONSOLE_ON */
/* #define DEBUG_ERR_CONSOLE_ON */

#include "timer.h"
#include "data.h"
#include "applicfg.h"

#define min_val(a,b) ((a<b)?a:b)

// function get TimerContext from CO_Data
static TimerContext* get_timer_ctx(CO_Data* d)
{
    if (d->timer_ctx != NULL) {
        return d->timer_ctx;
    } else {
        return NULL;
    }
}

/*!
** -------  Use this to declare a new alarm ------
**
** @param d
** @param id
** @param callback
** @param value
** @param period
**
** @return
**/
TIMER_HANDLE SetAlarm(CO_Data* d, UNS32 id, TimerCallback_t callback, TIMEVAL value, TIMEVAL period)
{
    TIMER_HANDLE row_number = 0;

    TimerContext* timer_ctx = get_timer_ctx(d);
    if (timer_ctx == NULL) {
        printf("SetAlarm: no timer context available in CO_Data\n");
        return TIMER_NONE;
    }
    s_timer_entry *row = timer_ctx->timers; /* get timer context from CO_Data */

    /* in order to decide new timer setting we have to run over all timer rows */
    for(row_number = 0; row_number <= (timer_ctx->last_timer_raw + 1) && row_number < MAX_NB_TIMER; ++row_number)
    {
        if (callback &&     /* if something to store */
           row->state == TIMER_FREE) /* and empty row */
        {    /* just store */
            TIMEVAL real_timer_value = min_val(value, TIMEVAL_MAX);
            TIMEVAL elapsed_time = getElapsedTime(timer_ctx);

            if (row_number == (timer_ctx->last_timer_raw + 1)) { ++timer_ctx->last_timer_raw; }

            /* set next wakeup alarm if new entry is sooner than others, or if it is alone */
            if ( (timer_ctx->total_sleep_time > elapsed_time) && (timer_ctx->total_sleep_time - elapsed_time > real_timer_value) )
            {
                timer_ctx->total_sleep_time = elapsed_time + real_timer_value;
                setTimer(real_timer_value, timer_ctx);
            }

            row->callback = callback;
            row->d = d;
            row->id = id;
            row->val = value + elapsed_time;
            row->interval = period;
            row->state = TIMER_ARMED;

            return row_number;
        }

        ++row;
    }

    return TIMER_NONE;
}

/*!
**  -----  Use this to remove an alarm ----
**
** @param handle
**
** @return
**/
TIMER_HANDLE DelAlarm(TIMER_HANDLE handle, TimerContext* timer_ctx)
{
    /* Quick and dirty. system timer will continue to be trigged, but no action will be preformed. */
    MSG_WAR(0x3320, "DelAlarm. handle = ", handle);
    if(handle != TIMER_NONE)
    {
        if(handle == timer_ctx->last_timer_raw)
        {
            timer_ctx->last_timer_raw--;
        }

        timer_ctx->timers[handle].state = TIMER_FREE;
    }
    return TIMER_NONE;
}

/*!
** ------  TimeDispatch is called on each timer expiration ----
**
**/
int tdcount=0;
void TimeDispatch(TimerContext* timer_ctx)
{
    TIMER_HANDLE i;
    TIMEVAL next_wakeup = TIMEVAL_MAX; /* used to compute when should normaly occur next wakeup */
    /* First run : change timer state depending on time */
    /* Get time since timer signal */
    UNS32 overrun = (UNS32)getElapsedTime(timer_ctx);

    TIMEVAL real_total_sleep_time = timer_ctx->total_sleep_time + overrun;

    s_timer_entry *row = timer_ctx->timers;

    for(i = 0; i <= timer_ctx->last_timer_raw; i++)
    {
        if (row->state & TIMER_ARMED) /* if row is active */
        {
            if (row->val <= real_total_sleep_time) /* to be trigged */
            {
                if (!row->interval) /* if simply outdated */
                {
                    row->state = TIMER_TRIG; /* ask for trig */
                }
                else /* or period have expired */
                {
                    /* set val as interval, with 32 bit overrun correction, */
                    /* modulo for 64 bit not available on all platforms     */
                    row->val = row->interval - (overrun % (UNS32)row->interval);
                    row->state = TIMER_TRIG_PERIOD; /* ask for trig, periodic */
                    /* Check if this new timer value is the soonest */
                    if(row->val < next_wakeup)
                    {
                        next_wakeup = row->val;
                    }
                }
            }
            else
            {
                /* Each armed timer value in decremented. */
                row->val -= real_total_sleep_time;

                /* Check if this new timer value is the soonest */
                if(row->val < next_wakeup)
                {
                    next_wakeup = row->val;
                }
            }
        }

        row++;
    }

    /* Remember how much time we should sleep. */
    timer_ctx->total_sleep_time = next_wakeup;

    /* Set timer to soonest occurence */
    setTimer(next_wakeup, timer_ctx);

    /* Then trig them or not. */
    row = timer_ctx->timers;
    for(i = 0; i <= timer_ctx->last_timer_raw; ++i)
    {
        if (row->state & TIMER_TRIG)
        {
            row->state &= ~TIMER_TRIG; /* reset trig state (will be free if not periodic) */
            if(row->callback)
            {
                (*row->callback)(row->d, row->id); /* trig ! */
            }
        }

        ++row;
    }
}
