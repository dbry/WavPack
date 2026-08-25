////////////////////////////////////////////////////////////////////////////
//                           **** WORKERS ****                            //
//                   Lightweight Worker Thread Manager                    //
//                Copyright (c) 2023 - 2025 David Bryant.                 //
//                          All Rights Reserved.                          //
//        Distributed under the BSD Software License (see LICENSE)        //
////////////////////////////////////////////////////////////////////////////

// workers.h

#ifndef WORKERS_H
#define WORKERS_H

#include <stdlib.h>
#include <stdint.h>

// these macros are to compare 32-bit unsigned job numbers (which can wrap)

#define A_BEFORE_B(A,B) (((A)-(B)) & 0x80000000)
#define A_AFTER_B(A,B) (((B)-(A)) & 0x80000000)

// This implements portable multithreading via typedefs and macros for either
// pthreads or native Windows threads. This is easy since the synchronization
// constructs we are using (condition variables and mutexes / critical
// sections) are available on both platforms with similar behavior.

#ifdef _WIN32

#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 /* for CONDITION_VARIABLE & co. */
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

typedef CONDITION_VARIABLE      wkr_condvar_t;
#define wkr_condvar_init(x)     InitializeConditionVariable(&x)
#define wkr_condvar_signal(x)   WakeAllConditionVariable(&x)
#define wkr_condvar_wait(x,y)   SleepConditionVariableCS(&x,&y,INFINITE)
#define wkr_condvar_delete(x)

typedef CRITICAL_SECTION        wkr_mutex_t;
#define wkr_mutex_init(x)       InitializeCriticalSection(&x)
#define wkr_mutex_obtain(x)     EnterCriticalSection(&x)
#define wkr_mutex_release(x)    LeaveCriticalSection(&x)
#define wkr_mutex_delete(x)     DeleteCriticalSection(&x)

typedef HANDLE                  wkr_thread_t;
#define wkr_thread_create(x,y,z) x=(HANDLE)_beginthreadex(NULL,0,y,z,0,NULL)
#define wkr_thread_join(x)      WaitForSingleObject(x,INFINITE)
#define wkr_thread_delete(x)    CloseHandle(x);
#define wkr_thread_exit(x)      _endthreadex(x);

#else

#include <pthread.h>

typedef pthread_cond_t          wkr_condvar_t;
#define wkr_condvar_init(x)     pthread_cond_init(&x,NULL);
#define wkr_condvar_signal(x)   pthread_cond_broadcast(&x)
#define wkr_condvar_wait(x,y)   pthread_cond_wait(&x,&y)
#define wkr_condvar_delete(x)   pthread_cond_destroy(&x)

typedef pthread_mutex_t         wkr_mutex_t;
#define wkr_mutex_init(x)       pthread_mutex_init(&x,NULL);
#define wkr_mutex_obtain(x)     pthread_mutex_lock(&x)
#define wkr_mutex_release(x)    pthread_mutex_unlock(&x)
#define wkr_mutex_delete(x)     pthread_mutex_destroy(&x)

typedef pthread_t               wkr_thread_t;
#define wkr_thread_create(x,y,z) do { if (pthread_create(&x,NULL,y,z)) x=0; } while (0)
#define wkr_thread_join(x)      pthread_join(x,NULL)
#define wkr_thread_delete(x)
#define wkr_thread_exit(x)      pthread_exit(x);

#endif

// This enum specifies the policies on using available worker threads
typedef enum {
    WaitForAvailableWorkerThread,       // wait for the next available worker thread and enqueue the job

    UseWorkerThreadOnlyIfAvailable,     // if there is an available worker thread, enqueue the job, otherwise
                                        // execute it on the current thread and return when job is done

    DontUseWorkerThread,                // do the job on the current thread regardless of available worker threads

    FailOnNoWorkerThreadAvailable       // return failure if no worker threads are available (work is not done)
                                        // this is the only policy that cannot block
} WorkerPolicy;

// These are the states that each worker thread goes through
typedef enum { Uninit, Ready, Running, Done, Quit } WorkerState;

typedef struct Workers Workers;

// Each worker thread owns one of these contexts during its lifetime

typedef struct {
    int worker_number;          // starting with 1 (0 is reserved for global structure)
    Workers *workers;           // pointer back to global structure
    WorkerState state;          // current state of the worker thread
    wkr_condvar_t condvar;      // these individual condvars are signaled by the background thread when the worker
                                // thread's "state" has been updated from "Ready" (either to "Running" or to "Quit")
    wkr_thread_t thread;        // this is the actual thread for the worker
    uint32_t job_number;        // this is the 32-bit incrementing non-zero job number (used for synchronization)
    int (*worker_function)(void*,void*); // this is the user-supplied function to actually perform the work
    void *worker_job;           // this is the user-supplied (and -defined) pointer to the work "data"
} WorkerInfo;

struct Workers {
    int worker_number;          // always 0 (to distinguish the structure from individual worker thread pointers)
    WorkerInfo *workers;        // pointer to the worker threads
    int num_workers;            // total number of worker threads
    int workers_ready;          // number of workers current in "Ready" state
    unsigned int job_number;    // next job number to be requested
    wkr_condvar_t condvar;      // this condvar is signaled by worker threads when they become "ready" which,
                                // except at initialization, also indicates that they just finished a job
    wkr_mutex_t mutex;          // global mutex protecting workers_ready count and worker's current states
};

#ifdef __cplusplus
extern "C" {
#endif

Workers *workersInit (int numWorkerThreads);
uint32_t workersEnqueueJob (Workers *cxt, int (*workerFunction)(void*,void*), void *WorkerJob, WorkerPolicy policy);
void workersWaitOnJob (Workers *cxt, uint32_t jobNumber);
int workersIsJobRunning (Workers *cxt, uint32_t jobNumber);
int workersNumAvailableWorkers (Workers *cxt);
int workersNumRunningJobs (Workers *cxt);
void workersWaitAllJobs (Workers *cxt);
void workersDeinit (Workers *cxt);
void workerSync (void *context);

#ifdef __cplusplus
}
#endif

#endif

