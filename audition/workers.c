////////////////////////////////////////////////////////////////////////////
//                           **** WORKERS ****                            //
//                   Lightweight Worker Thread Manager                    //
//                Copyright (c) 2023 - 2025 David Bryant.                 //
//                          All Rights Reserved.                          //
//        Distributed under the BSD Software License (see LICENSE)        //
////////////////////////////////////////////////////////////////////////////

// workers.c

// This module implements a very lightweight worker thread manager. Its purpose
// is to hide the mechanics (and possible pitfalls) of managing a worker thread
// pool and leave the user free to designing the application. It also abstracts
// away the differences between the Windows thread model and the pthreads model.
//
// Note that this is generally intended for situations where roughly the same
// work is split among multiple processor cores for performance reasons (like
// large mathematical calculations, simulations, or audio-video processing). It
// is NOT intended for splitting various unrelated or dissimilar tasks into
// various threads, but it MAY be suitable for that. I simply haven't thought
// too much about that application.

#include <stdio.h>

#include "workers.h"

#ifdef DEBUG
static unsigned int failures, enqueues, currents, last_job, unordered;  // debug info
#endif

// Each worker thread lives forever inside this function / loop. Both Windows API and
// pthreads API versions are provided. This is where the user-provided function that
// actually performs the work is called from.

#ifdef _WIN32
static unsigned WINAPI worker_thread (LPVOID param)
#else
static void *worker_thread (void *param)
#endif
{
    WorkerInfo *thread = param;
    Workers *global = thread->workers;

    while (1) {
        wkr_mutex_obtain (global->mutex);
        thread->state = Ready;
        global->workers_ready++;
        wkr_condvar_signal (global->condvar);       // signal that we're ready to work

        while (thread->state == Ready)              // wait for something to do
            wkr_condvar_wait (thread->condvar, global->mutex);

        wkr_mutex_release (global->mutex);

        if (thread->state == Quit)                  // break out if we're done, otherwise work...
            break;

        thread->worker_function (thread->worker_job, thread);

#ifdef DEBUG
        if (A_BEFORE_B (thread->job_number, last_job))
            unordered++;
        else
            last_job = thread->job_number;
#endif
    }

    wkr_thread_exit (0);
    return 0;
}

// This function is only called from within the user-provided function that performs the
// work. After this function is called (using the second void pointer passed into the
// work function) it is guaranteed that all previously enqueued jobs have run to
// completion and that the rest of the code in the job will run single-threaded (i.e.,
// no future job will be allowed into this section until after this job has run to
// completion). This is provided for applications that require that the results of the
// work be handled in the order that the jobs are enqueued, including things like
// race-free updating of global variables or writing the results to a file.

void workerSync (void *context)
{
    Workers *global = context;

    // First we handle the case where this was actually running on a worker thread. For
    // that case we must wait until all previous jobs are completed. However later jobs
    // and a job running on the user's thread can continue.

    if (global && global->worker_number) {
        WorkerInfo *info = context;
        int i;

        global = info->workers;
        wkr_mutex_obtain (global->mutex);

        for (i = 0; i < global->num_workers; ++i)
            while (global->workers [i].state == Running && A_BEFORE_B (global->workers [i].job_number, info->job_number))
                wkr_condvar_wait (global->condvar, global->mutex);

        wkr_mutex_release (global->mutex);
    }

    // The second case is where this is running on the user's thread, not on a worker thread.
    // For this case we must wait until ALL worker threads are completed.

    else if (global) {
        wkr_mutex_obtain (global->mutex);

        while (global->workers_ready < global->num_workers)
            wkr_condvar_wait (global->condvar, global->mutex);

        wkr_mutex_release (global->mutex);
    }

    // A final case is also handled where this is running without any worker threads at all,
    // indicated by the passed pointer being NULL. Obviously there's nothing to do then.
}

// Initialize the worker thread manager and spin up all the workers. There is no limit here
// imposed on the number of workers, but the underlying operating system and the machine's
// resources may certainly impose limits. Note that there is no issue creating more workers
// than the machine has cores, although the number of machine cores (or hyperthreads) is
// obviously a good starting point for experimentation.
//
// It is also possible to call this function specifying zero threads, which basically does
// nothing except return a NULL pointer. Interestingly though, this is valid and the rest
// of the manager's functionality is still perfectly usable and all requested jobs will
// simply be completed on the user's calling thread (which will block until the job is
// done). This is different from the case of specifying a single worker thread, which
// allows single jobs to run in the background while the user's thread immediately
// returns to the user.

Workers *workersInit (int numWorkerThreads)
{
    Workers *cxt;
    int i;

    if (!numWorkerThreads)  // if no worker threads, just return NULL pointer
        return NULL;        // (this is a valid use case and still works)

    // initialize the main structure of the worker manager

    cxt = calloc (1, sizeof (Workers));
    cxt->workers = calloc (cxt->num_workers = numWorkerThreads, sizeof (WorkerInfo));
    wkr_condvar_init (cxt->condvar);
    wkr_mutex_init (cxt->mutex);

    // initialize and start each worker thread

    for (i = 0; i < numWorkerThreads; ++i) {
        cxt->workers [i].workers = cxt;
        cxt->workers [i].worker_number = i + 1;
        wkr_condvar_init (cxt->workers [i].condvar);
        wkr_thread_create (cxt->workers [i].thread, worker_thread, &cxt->workers [i]);

        // gracefully handle failures in creating worker threads

        if (!cxt->workers [i].thread) {
            wkr_condvar_delete (cxt->workers [i].condvar);
            cxt->num_workers = i;
            break;
        }
    }

    if (!cxt->num_workers) {    // if we failed to start any workers, free the array
        free (cxt->workers);
        cxt->workers = NULL;
        wkr_mutex_delete (cxt->mutex);
        wkr_condvar_delete (cxt->condvar);
        free (cxt);
        return NULL;
    }

    // wait for all worker threads to get to the "Ready" state

    wkr_mutex_obtain (cxt->mutex);

    while (cxt->workers_ready < cxt->num_workers)
        wkr_condvar_wait (cxt->condvar, cxt->mutex);

    wkr_mutex_release (cxt->mutex);

    return cxt;
}

// This is the function that enqueues a job to be completed, potentially by a worker thread
// or, in some situations, on the calling thread (see "policy" below).
//
// The arguments are:
//
// cxt:             Context pointer returned by workersInit();
//
// WorkerFunction:  Pointer to the function that will be called to actually do the work. The
//                  arguments are two void pointers. The first pointer is a pointer to the
//                  user-defined area that describes the work and possibly provides a place
//                  to return the results of the work. The second pointer passed in is an
//                  opaque pointer used by the worker function to call the workerSync()
//                  function (if desired).
//
// WorkerJob:       This is an opaque (to the manager, anyway) pointer that is user-defined
//                  and descibes the work to be performed. It may contain fields to return
//                  results or status also if required, and also things like a "stop" flag
//                  for terminating early. The area pointed to is owned by the caller. It
//                  obviously may contain pointers to other areas also owned by the caller.
//                  It might make sense to have the worker function free the pointer on
//                  completetion, or it might make sense for the caller to free it when the
//                  results have been extracted from it. In any event, this is the pointer
//                  that is passed into the worker function as the first argument.
//
// policy:          This enum controls the execution policy for the individual job, thus:
//
//     WaitForAvailableWorkerThread:    This is the most common case and simply requests that the
//                                      job be given to the next available worker thread. The
//                                      function will block if there isn't a worker thread available
//                                      but will otherwise return immediately. It will not block and
//                                      execute the job on the user's thread unless there are no
//                                      worker threads at all (the numWorkers == zero case).
//
//     UseWorkerThreadOnlyIfAvailable:  Similar to the above case, except that if there is no
//                                      available worker thread the job is executed on the caller's
//                                      thread (which blocks, obviously). This policy might be useful
//                                      if there are very few worker threads or for the last job in a
//                                      batch of jobs where we have to wait until the others have
//                                      completed anyway (so there's nothing else for the user's
//                                      thread to do).
//
//     DontUseWorkerThread:             Execute the job on the current thread regardless of available
//                                      worker threads.
//
//     FailOnNoWorkerThreadAvailable:   Return failure (0) and do nothing if no worker threads are
//                                      currently available. This is the only policy that cannot block
//                                      and the only policy that can fail. It's also the only policy
//                                      that can result in a job not being started. Note that in the
//                                      special numWorkers == zero case this policy acts like all the
//                                      others and simply executes the job on the caller's thread and
//                                      returns 1.
//
// returns:         Zero for failure, otherwise a non-zero job number. In the numWorkers == zero /
//                  NULL context case, 1 is returned after the task executes to completetion.
//
// Note that this is nominally thread-safe and could conceivably be safely called from multiple threads.
// However, use caution as this breaks some of the functionality. For example, if policies are used
// that result in jobs being done on the user's thread, having multiple jobs running on the user's
// thread might happen, and this would break the workerSync() functionality. Also, if this is being
// called on multiple threads then calling a function like workersNumAvailableWorkers() might end up
// indicating a worker was available which might be no longer be available before trying to enqueue a
// job on the same thread.

unsigned int workersEnqueueJob (Workers *cxt, int (*workerFunction)(void *, void *), void *workerJob, WorkerPolicy policy)
{
    uint32_t job_number;
    int i;

    // handle the unitialized numWorkers == zero case by simply executing the job and returning zero

    if (!cxt) {
        workerFunction (workerJob, cxt);
        return 1;
    }

    wkr_mutex_obtain (cxt->mutex);

    // handle the FailOnNoWorkerThreadAvailable policy by returning zero if there are no workers available

    if (!cxt->workers_ready && policy == FailOnNoWorkerThreadAvailable) {
#ifdef DEBUG
        failures++;
#endif
        wkr_mutex_release (cxt->mutex);
        return 0;
    }

    while (!(job_number = cxt->job_number++));      // get the non-zero job number and increment for the next

    // this handles the case where we might execute the job right here on the user's thread

    if (policy != WaitForAvailableWorkerThread)
        if (policy == DontUseWorkerThread || (!cxt->workers_ready && policy == UseWorkerThreadOnlyIfAvailable)) {
#ifdef DEBUG
            currents++;
#endif
            wkr_mutex_release (cxt->mutex);
            workerFunction (workerJob, cxt);

#ifdef DEBUG
            if (A_BEFORE_B (job_number, last_job))
                unordered++;
            else
                last_job = job_number;
#endif
            return job_number;
        }

    // if we get here then we are going to enqueue the job, so first potentially wait until there is an available worker

    while (!cxt->workers_ready)
        wkr_condvar_wait (cxt->condvar, cxt->mutex);

    // there's definitely a worker available, so loop through the individual worker thread looking for one "Ready",
    // then enqueue the job, set the worker's state to "Running", and signal the worker's thread

    for (i = 0; i < cxt->num_workers; ++i)
        if (cxt->workers [i].state == Ready) {
            cxt->workers [i].job_number = job_number;
            cxt->workers [i].worker_job = workerJob;
            cxt->workers [i].worker_function = workerFunction;
            cxt->workers [i].state = Running;
            wkr_condvar_signal (cxt->workers [i].condvar);
            cxt->workers_ready--;
#ifdef DEBUG
            enqueues++;
#endif
            break;
        }

    wkr_mutex_release (cxt->mutex);
    return job_number;
}

// Determine whether a specific job number is running, and return TRUE if so. The job number is
// the non-zero value returned by workersEnqueueJob (). Note that if all the worker functions
// are calling workerSync(), then a FALSE return from this function would indicate that ALL
// jobs before the specified one have also completed. Note that this will not apply to a job
// running on the user's thread (but of course that would indicate that multiple threads
// were calling into the manager).

int workersIsJobRunning (Workers *cxt, uint32_t jobNumber)
{
    int retval = 0;

    if (cxt) {
        int i;

        wkr_mutex_obtain (cxt->mutex);

        for (i = 0; i < cxt->num_workers; ++i)
            if (cxt->workers [i].state == Running && cxt->workers [i].job_number == jobNumber) {
                retval = 1;
                break;
            }

        wkr_mutex_release (cxt->mutex);
    }

    return retval;
}

// Determine whether a specific job number is running, and if so block until it completes. The job
// number is the non-zero value returned by workersEnqueueJob(). Note that if all the worker
// functions are calling workerSync(), then this function would block until ALL jobs before the
// specified one have also completed. Note that this will not apply to a job running on the user's
// thread (but of course that would indicate that multiple threads were calling into the manager).

void workersWaitOnJob (Workers *cxt, uint32_t jobNumber)
{
    if (cxt) {
        int i;

        wkr_mutex_obtain (cxt->mutex);

        for (i = 0; i < cxt->num_workers; ++i)
            while (cxt->workers [i].state == Running && cxt->workers [i].job_number == jobNumber)
                wkr_condvar_wait (cxt->condvar, cxt->mutex);

        wkr_mutex_release (cxt->mutex);
    }
}

// Block until all jobs have completed, not counting any job(s) running on the user's thread.

void workersWaitAllJobs (Workers *cxt)
{
    if (cxt) {
        wkr_mutex_obtain (cxt->mutex);

        while (cxt->workers_ready < cxt->num_workers)
            wkr_condvar_wait (cxt->condvar, cxt->mutex);

        wkr_mutex_release (cxt->mutex);
    }
}

// Return the number of jobs currently running on worker threads. This does not include any job(s)
// running on the user's thread(s).

int workersNumRunningJobs (Workers *cxt)
{
    int retval = 0;

    if (cxt) {
        wkr_mutex_obtain (cxt->mutex);
        retval = cxt->num_workers - cxt->workers_ready;
        wkr_mutex_release (cxt->mutex);
    }

    return retval;
}

// Return the number of worker threads currently available to accept jobs and do work.

int workersNumAvailableWorkers (Workers *cxt)
{
    int retval = 0;

    if (cxt) {
        wkr_mutex_obtain (cxt->mutex);
        retval = cxt->workers_ready;
        wkr_mutex_release (cxt->mutex);
    }

    return retval;
}

// Destroy the specified instance of the workers thread manager. This includes spinning down the
// worker threads and freeing all resources consumed by the manager. It's probably a good idea
// to not do this until all the workers are in the "Ready" state (by, for example, calling
// workersWaitAllJobs()), but this would normally be the case in well-designed application.
// After calling this function, the context pointer should not be reused.

void workersDeinit (Workers *cxt)
{
    if (cxt) {
        int i;

#ifdef DEBUG
        printf ("total jobs = %u, failures = %u, enqueues = %u, currents = %u, unordered = %u\n",
            cxt->job_number - 1, failures, enqueues, currents, unordered);
#endif

        for (i = 0; i < cxt->num_workers; ++i) {
            wkr_mutex_obtain (cxt->mutex);
            cxt->workers [i].state = Quit;
            wkr_condvar_signal (cxt->workers [i].condvar);
            wkr_mutex_release (cxt->mutex);
            wkr_thread_join (cxt->workers [i].thread);
            wkr_thread_delete (cxt->workers [i].thread);
            wkr_condvar_delete (cxt->workers [i].condvar);
        }

        free (cxt->workers);
        cxt->workers = NULL;
        wkr_mutex_delete (cxt->mutex);
        wkr_condvar_delete (cxt->condvar);
        free (cxt);
    }
}
