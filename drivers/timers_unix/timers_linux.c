#include <stdlib.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

#include "applicfg.h"
#include "timer.h"
#include "can_driver.h"

/* We assume that ReceiveLoop_task_proc is always the same */
// void (*unixtimer_ReceiveLoop_task_proc)(CAN_PORT) = NULL;

#define maxval(a,b) ((a>b)?a:b)

void TimerCleanup(TimerContext* timer_ctx)
{
    /* only used in realtime apps */
}

void EnterMutex(TimerContext* timer_ctx)
{
    if(pthread_mutex_lock(&timer_ctx->mutex))
    {
        fprintf(stderr, "pthread_mutex_lock() failed\n");
    }
}

void LeaveMutex(TimerContext* timer_ctx)
{
    if(pthread_mutex_unlock(&timer_ctx->mutex))
    {
        fprintf(stderr, "pthread_mutex_unlock() failed\n");
    }
}

void* timer_notify_thr(void* arg)
{
    TimerContext* timer_ctx = (TimerContext*)arg;
    while (1)
    {
        uint64_t exp = 0;
        
        int ret = read(timer_ctx->timerfd, &exp, sizeof(uint64_t));
        
        if (ret == sizeof(uint64_t))
        {
            if(gettimeofday(&timer_ctx->last_sig, NULL))
            {
                perror("gettimeofday()");
            }

            EnterMutex(timer_ctx);
            TimeDispatch(timer_ctx);
            LeaveMutex(timer_ctx);
        }
    }
}

void TimerInit(TimerContext* timer_ctx)
{
    // Take first absolute time ref.
    if(gettimeofday(&timer_ctx->last_sig, NULL))
    {
        perror("gettimeofday()");
    }

    timer_ctx->timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_ctx->timerfd == -1)
    {
        perror("timer_create()");
    }

    struct itimerspec itime;
    itime.it_value.tv_sec     = 0;
    itime.it_value.tv_nsec    = 0;
    itime.it_interval.tv_sec  = 0;
    itime.it_interval.tv_nsec = 0;
    // stop timer at first
    if (timerfd_settime(timer_ctx->timerfd, 0, &itime, NULL) == -1)
    {
        perror("timerfd_settime()");
    }

    if(pthread_create(&timer_ctx->timer_tid, NULL, timer_notify_thr, (void*)timer_ctx))
    {
        perror("pthread_create()");
    }
}

void StopTimerLoop(TimerContext* timer_ctx, TimerCallback_t exitfunction)
{
    EnterMutex(timer_ctx);

    struct itimerspec itime;
    itime.it_value.tv_sec     = 0;
    itime.it_value.tv_nsec    = 0;
    itime.it_interval.tv_sec  = 0;
    itime.it_interval.tv_nsec = 0;
    /* stop timer */
    if (timerfd_settime(timer_ctx->timerfd, 0, &itime, NULL) == -1)
    {
        perror("timerfd_settime()");
    }
    close(timer_ctx->timerfd);

    timer_ctx->timerfd = -1;

    exitfunction(NULL,0);
    LeaveMutex(timer_ctx);
}

void StartTimerLoop(TimerContext* timer_ctx, TimerCallback_t init_callback)
{
    EnterMutex(timer_ctx);
    // At first, TimeDispatch will call init_callback.
    SetAlarm(NULL, 0, init_callback, 0, 0);
    LeaveMutex(timer_ctx);
}

/**
 * Enter in realtime and start the CAN receiver loop
 * @param port
 */
void* unixtimer_canReceiveLoop(void* port)
{
        /* signal handling removed (unused) */
    ((CANPort*)port)->unixtimer_ReceiveLoop_task_proc((CAN_PORT)port);

    return NULL;
}

void CreateReceiveTask(CAN_PORT port, TASK_HANDLE* Thread, void* ReceiveLoopPtr)
{
    ((CANPort*)port)->unixtimer_ReceiveLoop_task_proc = ReceiveLoopPtr;
    if(pthread_create(Thread, NULL, unixtimer_canReceiveLoop, (void*)port))
    {
        perror("pthread_create()");
    }
}

void WaitReceiveTaskEnd(TASK_HANDLE *Thread)
{
    if(pthread_cancel(*Thread))
    {
        perror("pthread_cancel()");
    }

    if(pthread_join(*Thread, NULL))
    {
        perror("pthread_join()");
    }
}

void setTimer(TIMEVAL value, TimerContext* timer_ctx)
{
//    printf("setTimer(TIMEVAL value=%d)\n", value);
    // TIMEVAL is us whereas setitimer wants ns...
    long tv_nsec = 1000 * (maxval(value,1)%1000000);
    time_t tv_sec = value/1000000;
    struct itimerspec timerValues;
    timerValues.it_value.tv_sec = tv_sec;
    timerValues.it_value.tv_nsec = tv_nsec;
    timerValues.it_interval.tv_sec = 0;
    timerValues.it_interval.tv_nsec = 0;

    if (timerfd_settime(timer_ctx->timerfd, 0, &timerValues, NULL) == -1)
    {                
        perror("timerfd_settime");
    }
}

TIMEVAL getElapsedTime(TimerContext* timer_ctx)
{
    struct timeval p;
    if(gettimeofday(&p,NULL)) 
    {
        perror("gettimeofday()");
    }
//    printf("getCurrentTime() return=%u\n", p.tv_usec);
    return (p.tv_sec - timer_ctx->last_sig.tv_sec)* 1000000 + p.tv_usec - timer_ctx->last_sig.tv_usec;
}
