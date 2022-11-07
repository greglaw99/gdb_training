/**
 * \file
 * \brief Test our own mutex implementation, and used as debuggee for
 * tests of threading, e.g. tests of thread freezing
 *
 * Launches a bunch of threads that do a bunch of ops at random on a bunch
 * of doubly-linked lists.
 *
 * Also sends itself the odd signal to check the mutex ops are resistant
 * against EINTR
 *
 * Be cautious recording this as a debuggee, when compiled in debug mode,
 * our mutexes call gettid in every mutex_lock or mutex_unlock, which
 * on many computers is too many for a default-sized event log.
 *
 * Also used to test thread freezing in test784_fn family, and for a few
 * other tests that need a threaded debuggee. It may in future be more
 * useful to split implementation used for those tests into a separate
 * file so they can evolve independently, and possibly use pthread mutexes
 */

#include "util/ensure.h"
#include "util/types/list.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#if !defined(_GNU_SOURCE) || !defined(__GLIBC__) || __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 30)
static pid_t
gettid(void)
{
    return syscall(SYS_gettid);
}
#endif

#include "util/better_libc/sync/mutex.h"

enum {MAX_LISTS=10};
enum {MAX_THREADS=10};
char spinners [MAX_THREADS];

struct list {
    list_t     list;
    mutex_t    lock;
    unsigned   node_count;
} lists [MAX_LISTS];

/* These variables are required by the signal handler in order to disable signals when the test
 * duration has elapsed. These are only assigned by the main thread. */
static int duration = 30;
static int timer = ITIMER_REAL;
static time_t end_time;

static void
do_alarm(int sig)
{
    static unsigned stuck_count = 0;
    static char prev_spinners [MAX_THREADS];
    (void)sig;

    write (1, "\r*", 3);

    /* Make sure spinners has changed since last time we were called.
     * Note - this alarm is set via setitimer with ITIMER_VIRTUAL, so we should
     * have received sufficient processor time since last time.
     */
    if (memcmp (prev_spinners, spinners, sizeof spinners))
        stuck_count = 0;
    else
        stuck_count++;

    /* Stuck many times in a row probably means live-lock - we're getting signals
     * quicker than the signal handler can run, so the main app makes no
     * progress (stuck a few times probably just means we didn't get any CPU for
     * a while)
     */
    ENSURE(stuck_count < 1000, "stuck!");

    /* Record prev_spinners for next time */
    memcpy (prev_spinners, spinners, sizeof spinners);

    /* Now let's try to get the mutexes.  Note that this may well fail due to
     * the mutex already being locked by this thread.  Important to do
     * mutex_trylock not mutex_lock from this signal handler, as otherwise it
     * could result in deadlock.  e.g. thread T1 takes mutex A and T2 takes
     * mutex B.  Then T1 gets a signal, and blocks on mutex B.  Then T2 gets a
     * signal, and blocks on mutex A.  Deadlock.
     */
    for (int i = 0; i < MAX_LISTS; i++)
    {
        int r = mutex_trylock (&lists [i].lock);
        if (r)
        {
            ENSURE(r == -EBUSY || r == -EDEADLK, "r=%d(%s)", r, strerror(-r));
        }
        else
        {
            r = mutex_unlock (&lists [i].lock);
            ENSURE(r == 0, "r=%d", r);

            /* Let's try again - this time should report EPERM as we no longer
             * have the lock
             */
            r = mutex_unlock (&lists [i].lock);
            ENSURE(r == -EPERM, "%d", r);
        }
    }

    /* Cancel the timer if the test runs for twice the specified duration of the test. This prevents
     * the test from hanging in the case where the signals are sent faster than the process is able
     * to process them under recording. This is necessary to prevent timeouts with self-recording,
     * see #21725. */
    int timer_end_time = end_time + duration;
    if (timer_end_time < time(0))
    {
        struct itimerval it;
        bzero(&it, sizeof(struct itimerval));
        ENSURE_PERROR(setitimer(timer, &it, NULL) == 0);
    }
}

static void*
looper(void *arg)
{
    unsigned long thread_no = (unsigned long)arg;
    int r;

    while (true)
    {
        list_t *link = NULL;

        /* The libc random() takes a lock.  We need therefore to call it with
         * the mutex held, otherwise we can deadlock because:
         * - Thread A takes mutex, then preempted
         * - Thread B calls random(), which takes the libc lock
         * - Signal handler fires on thread B, which tries to take mutex.
         * - Thread A gets rescheduled, calls random(), tries to take libc lock
         *
         * ... deadlock!
         *
         * i.e. this is a classic deadlock with two threads trying to locks in
         * different orders.  To overcome, we either need to call random()
         * always with the mutex held, or never with the mutex held.  In the
         * world of the BFM, this translates to saying all libc functions must
         * be called with the BFM held.  However, here we have 10 mutexes, so
         * it's not so simple.  Instead we'll make sure we call random() before
         * the mutex is taken.
         */

        unsigned i = random() % MAX_LISTS;
        unsigned count = random();

        spinners [thread_no]++;

        /* At random we: i) add a node to the list; ii) count the nodes in the
         * list; iii) remove a node from the list, iv) do nothing.
         */
        switch (random() % 4)
        {
            case 0:
                //printf ("Add node to list %d\n", i);
                /* Add a node to the list */
                link = malloc (sizeof *link);
                if (!link)
                {
                  fprintf (stderr, "Out of memory\n");
                  exit (1);
                }

                r = mutex_lock (&lists [i].lock);
                ENSURE(r == 0, "r=%d", r);

                list_addafter (&lists [i].list, link);
                lists [i].node_count++;

                r = mutex_unlock (&lists [i].lock);
                ENSURE(r == 0, "r=%d", r);

                break;

            case 1:
                /* Count the nodes on the list */
                //printf ("Count nodes on list %d...", i);
                count = 0;
                r = mutex_lock (&lists [i].lock);
                ENSURE(r == 0, "r=%d", r);

                for (link = lists[i].list.next; link != &lists[i].list; link = link->next)
                    count++;

                //printf ("I counted %d\n", count);

                if (count != lists[i].node_count) {
                    fprintf (stderr, "Error on list %d: Expected to find %d nodes, found %d\n",
                             i, lists [i].node_count, count);
                    exit (2);
                }

                r = mutex_unlock (&lists [i].lock);
                ENSURE(r == 0, "r=%d", r);

                break;

            case 2:
                /* Remove a random node from the list */
                r = mutex_lock (&lists [i].lock);
                ENSURE(r == 0, "r=%d", r);

                //printf ("Remove node from list %d...", i);

                if (lists [i].node_count) {
                    count %= lists [i].node_count;
      	            for (link = lists[i].list.next; count; link = link->next)
                        count--;

                    list_remove (link);
                    lists[i].node_count--;

                    r = mutex_unlock (&lists [i].lock);
                    ENSURE(r == 0, "r=%d", r);

                    /* Don't actually free it, just poison it.  Obviously this leaks like
                     * crazy, but it increases the chances that we'll detect any problem
                     * with the locks -- i.e. no ABAs masking problems (note these tests
                     * wouldn't detect any /problems/ associated with ABA)
                     */
                    link->next = (void*)0xdeadf00d;
                    link->prev = (void*)0xabadcafe;
                    //printf ("removed\n");
                }
                else {
                    //printf ("nothing to remove\n");
                    r = mutex_unlock (&lists [i].lock);
                    ENSURE(r == 0, "r=%d", r);
                }

                break;

            case 3:
                /* Do nothing */
                r = mutex_lock (&lists [i].lock);
                ENSURE(r == 0, "r=%d", r);
                r = mutex_unlock (&lists [i].lock);
                ENSURE(r == 0, "r=%d", r);
                break;

            default:
                ENSURE(0, "r=%d", r);
        }
    }

    fprintf( stderr, "thread exiting: %i\n", gettid());
}

static void
finish( void)
/* a convenient place to put a breakpoint. */
{
}

static void
threads_running( void)
/* a convenient place to put a breakpoint. */
{
}

int
main(int argc, char *argv[])
{
    int sig = SIGALRM;
    static pthread_t threads [MAX_THREADS];
    struct sigaction act;
    struct itimerval it = {.it_value = {.tv_sec = 0, .tv_usec = 100*1000}};
    it.it_interval = it.it_value;

    if (argc > 1)
    {
        duration = atoi (argv[1]);
        if (duration == 0)
            duration = 30;

        if (argc > 2) {
            if (!strcmp (argv[2], "oneshot")) {
                printf ("test182.c: oneshot mode\n");
                it.it_interval.tv_sec = 0;
                it.it_interval.tv_usec = 0;
            }
            else if (!strcmp (argv[2], "vtalrm")) {
                printf ("test182.c: using SIGVTALRM/ITIMER_VIRTUAL\n");
                sig = SIGVTALRM;
                timer = ITIMER_VIRTUAL;

                if (argc > 3) {
                    it.it_interval.tv_usec = atoi (argv [3]);
                    printf ("interval set to %lu\n", it.it_interval.tv_usec);
                }
            }
            else {
                it.it_interval.tv_usec = atoi (argv [2]);
                printf ("interval set to %lu\n", it.it_interval.tv_usec);
            }
        }
    }

    printf ("Initialising...\n");

    /* Important to set up the mutexes prior to triggering the alarm as our
     * SIGALRM signal handler relies on the mutexes being initialised. */
    for (int i = 0; i < MAX_LISTS; i++)
    {
        int r = mutex_init(&lists[i].lock, MUTEX_OWNER_CHECK);
        ENSURE(r == 0, "i=%d, mutex_init() returned %i (%s)\n", i, r, strerror(-r));
        list_init (&lists [i].list);
        lists [i].node_count = 0;
    }


    /* Assign the end time before setting up the alarm because the signal handler depends on it. */
    end_time = time(0) + duration;

    /* Set up a signal handler that will fire 100 times per sec.
     * This generates EINTR return codes from FUTEX_WAIT
     */
    act.sa_handler = do_alarm;
    act.sa_flags = 0;
    sigemptyset (&act.sa_mask);
    ENSURE_PERROR(sigaction(sig, &act, NULL) == 0);
    ENSURE_PERROR(setitimer(timer, &it, NULL) == 0);

    /* Turn off all buffering on stdout so that our funky display of a
     * "spinner" works (coz we don't output newlines as the test runs)
     */
    ENSURE_PERROR(setvbuf(stdout, NULL, _IONBF, 0) == 0);

    printf ("Starting threads...\n");
    /* Start the threads */
    for (intptr_t i = 0; i < MAX_THREADS; i++)
    {
        ENSURE_PTHREADS(pthread_create (&threads [i], NULL, looper, (void*)i));
    }
    threads_running();

    /* Wait while the tests run, printing progress on the screen */
    printf ("Testing (test will run for %ds):\n", duration);
    for (int i = 0; i < MAX_THREADS; i++)
    {
        printf ("   ");
    }

    while (end_time > time(0))
    {
        printf ("\r %4ds ", (int)(end_time - time(0)));
        for (int i = 0; i < MAX_THREADS; i++)
        {
           printf (" %c ", (spinners [i] % 26) + 65);
        }

        usleep (100*1000);  /* 10x frames a second */

    }

    /* Cancel the timer. This is important lest the SIGALRM interrupts the printf below and a test is
     * relying on its output. (See #16995) */
    bzero(&it, sizeof(struct itimerval));
    ENSURE_PERROR(setitimer(timer, &it, NULL) == 0);

    /* If we get here the test has run to duration. If our mutexes failed
     * we would have got a SEGV or deadlock or something earlier.
     *
     * thread-freezing tests typically stop execution before this point
     */
    finish();
    printf ("\n\nAll done!\n");
    fflush (stdout);

    return 0;
}
