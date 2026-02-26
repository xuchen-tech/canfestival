#include <stdlib.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

#include "applicfg.h"
#include "timer.h"

static pthread_mutex_t CanFestival_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct timeval last_sig;

static int iTimerFD = -1;

static pthread_t iTimeThrId;
static int iTimerUsers = 0;
static int iTimerThreadRunning = 0;
static int iTimerThreadStop = 0;


void TimerCleanup(void)
{
    int timerfd_to_close = -1;
    int need_join = 0;

    EnterMutex();

    if (iTimerUsers <= 0)
    {
        LeaveMutex();
        return;
    }

    iTimerUsers--;
    if (iTimerUsers > 0)
    {
        LeaveMutex();
        return;
    }

    iTimerThreadStop = 1;
    timerfd_to_close = iTimerFD;
    iTimerFD = -1;
    need_join = iTimerThreadRunning;

    LeaveMutex();

    if (timerfd_to_close != -1)
    {
        close(timerfd_to_close);
    }

    if (need_join)
    {
        if(pthread_join(iTimeThrId, NULL))
        {
            perror("pthread_join()");
        }
    }

    EnterMutex();
    iTimerThreadRunning = 0;
    iTimerThreadStop = 0;
    LeaveMutex();
}

void EnterMutex(void)
{
    if(pthread_mutex_lock(&CanFestival_mutex))
    {
        fprintf(stderr, "pthread_mutex_lock() failed\n");
    }
}

void LeaveMutex(void)
{
    if(pthread_mutex_unlock(&CanFestival_mutex))
    {
        fprintf(stderr, "pthread_mutex_unlock() failed\n");
    }
}


void* timer_notify_thr(void* arg)
{
    (void)arg;
    while (1)
    {
        int timerfd = iTimerFD;
        if (timerfd < 0)
        {
            if (iTimerThreadStop)
            {
                break;
            }
            usleep(1000);
            continue;
        }

        uint64_t exp = 0;
        
        int ret = read(timerfd, &exp, sizeof(uint64_t));
        
        if (ret == sizeof(uint64_t))
        {
            if(gettimeofday(&last_sig, NULL))
            {
                perror("gettimeofday()");
            }

            EnterMutex();
            TimeDispatch();
            LeaveMutex();
        }
        else if (ret < 0 && iTimerThreadStop)
        {
            break;
        }
    }

    return NULL;
}


void TimerInit(void)
{
    EnterMutex();
    if (++iTimerUsers > 1)
    {
        LeaveMutex();
        return;
    }

    // Take first absolute time ref.
    if(gettimeofday(&last_sig, NULL))
    {
        perror("gettimeofday()");
    }


    iTimerFD = timerfd_create(CLOCK_MONOTONIC, 0);
    if (iTimerFD == -1)
    {
        perror("timer_create()");
        iTimerUsers = 0;
        LeaveMutex();
        return;
    }

    struct itimerspec itime;
    itime.it_value.tv_sec     = 0;
    itime.it_value.tv_nsec    = 0;
    itime.it_interval.tv_sec  = 0;
    itime.it_interval.tv_nsec = 0;
    // stop timer at first
    if (timerfd_settime(iTimerFD, 0, &itime, NULL) == -1)
    {
        perror("timerfd_settime()");
    }

    if(pthread_create(&iTimeThrId, NULL, timer_notify_thr, NULL))
    {
        perror("pthread_create()");
        close(iTimerFD);
        iTimerFD = -1;
        iTimerUsers = 0;
        LeaveMutex();
        return;
    }

    iTimerThreadRunning = 1;
    iTimerThreadStop = 0;
    LeaveMutex();
}

void StopTimerLoop(TimerCallback_t exitfunction)
{
    EnterMutex();

    struct itimerspec itime;
    itime.it_value.tv_sec     = 0;
    itime.it_value.tv_nsec    = 0;
    itime.it_interval.tv_sec  = 0;
    itime.it_interval.tv_nsec = 0;
    /* stop timer */
    if (timerfd_settime(iTimerFD, 0, &itime, NULL) == -1)
    {
        perror("timerfd_settime()");
    }
    if(exitfunction)
    {
        exitfunction(NULL,0);
    }
    LeaveMutex();
}

void StartTimerLoop(TimerCallback_t init_callback)
{
    EnterMutex();
    // At first, TimeDispatch will call init_callback.
    SetAlarm(NULL, 0, init_callback, 0, 0);
    LeaveMutex();
}

void canReceiveLoop_signal(int sig)
{
}
typedef struct {
    CAN_PORT port;
    void (*receive_proc)(CAN_PORT);
} s_receive_task_ctx;

/**
 * Enter in realtime and start the CAN receiver loop
 * @param port
 */
void* unixtimer_canReceiveLoop(void* port)
{
    s_receive_task_ctx* ctx = (s_receive_task_ctx*)port;

    if(ctx && ctx->receive_proc)
    {
        ctx->receive_proc(ctx->port);
    }

    if(ctx)
    {
        free(ctx);
    }

    return NULL;
}

void CreateReceiveTask(CAN_PORT port, TASK_HANDLE* Thread, void* ReceiveLoopPtr)
{
    s_receive_task_ctx* ctx = (s_receive_task_ctx*)malloc(sizeof(s_receive_task_ctx));
    if(!ctx)
    {
        perror("malloc()");
        return;
    }

    ctx->port = port;
    ctx->receive_proc = (void (*)(CAN_PORT))ReceiveLoopPtr;

    if(pthread_create(Thread, NULL, unixtimer_canReceiveLoop, (void*)ctx))
    {
        perror("pthread_create()");
        free(ctx);
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

#define maxval(a,b) ((a>b)?a:b)
void setTimer(TIMEVAL value)
{
//    printf("setTimer(TIMEVAL value=%d)\n", value);
    if (iTimerFD < 0)
    {
        return;
    }
    // TIMEVAL is us whereas setitimer wants ns...
    long tv_nsec = 1000 * (maxval(value,1)%1000000);
    time_t tv_sec = value/1000000;
    struct itimerspec timerValues;
    timerValues.it_value.tv_sec = tv_sec;
    timerValues.it_value.tv_nsec = tv_nsec;
    timerValues.it_interval.tv_sec = 0;
    timerValues.it_interval.tv_nsec = 0;

    if (timerfd_settime(iTimerFD, 0, &timerValues, NULL) == -1)
    {                
        perror("timerfd_settime");
    }
}

TIMEVAL getElapsedTime(void)
{
    struct timeval p;
    if(gettimeofday(&p,NULL)) 
    {
        perror("gettimeofday()");
    }
//    printf("getCurrentTime() return=%u\n", p.tv_usec);
    return (p.tv_sec - last_sig.tv_sec)* 1000000 + p.tv_usec - last_sig.tv_usec;
}
