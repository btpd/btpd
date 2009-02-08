#include <time.h>

#include "evloop.h"
#include "timeheap.h"

#if defined(HAVE_CLOCK_MONOTONIC)

#ifdef CLOCK_MONOTONIC_FAST
#define TIMER_CLOCK CLOCK_MONOTONIC_FAST
#else
#define TIMER_CLOCK CLOCK_MONOTONIC
#endif

int
evtimer_gettime(struct timespec *ts)
{
    return clock_gettime(TIMER_CLOCK, ts);
}

#elif defined(HAVE_MACH_ABSOLUTE_TIME)

#include <mach/mach_time.h>

int
evtimer_gettime(struct timespec *ts)
{
    uint64_t nsecs;
    static double nsmul;
    static mach_timebase_info_data_t nsratio = { 0, 0 };
    if (nsratio.denom == 0) {
        mach_timebase_info(&nsratio);
	nsmul = (double)nsratio.numer / nsratio.denom;
    }
    nsecs = mach_absolute_time() * nsmul;
    ts->tv_sec = nsecs / 1000000000ULL;
    ts->tv_nsec = nsecs - ts->tv_sec * 1000000000ULL;
    return 0;
}

#else
#error No supported time mechanism
#endif

static struct timespec
addtime(struct timespec a, struct timespec b)
{
    struct timespec ret;
    ret.tv_sec = a.tv_sec + b.tv_sec;
    ret.tv_nsec = a.tv_nsec + b.tv_nsec;
    if (ret.tv_nsec >= 1000000000) {
        ret.tv_sec  += 1;
        ret.tv_nsec -= 1000000000;
    }
    return ret;
}

static struct timespec
subtime(struct timespec a, struct timespec b)
{
    struct timespec ret;
    ret.tv_sec = a.tv_sec - b.tv_sec;
    ret.tv_nsec = a.tv_nsec - b.tv_nsec;
    if (ret.tv_nsec < 0) {
        ret.tv_sec  -= 1;
        ret.tv_nsec += 1000000000;
    }
    return ret;
}

void
evtimer_init(struct timeout *h, evloop_cb_t cb, void *arg)
{
    h->cb = cb;
    h->arg = arg;
    h->th.i = -1;
    h->th.data = h;
}

int
evtimer_add(struct timeout *h, struct timespec *t)
{
    struct timespec now, sum;
    evtimer_gettime(&now);
    sum = addtime(now, *t);
    if (h->th.i == -1)
        return timeheap_insert(&h->th, &sum);
    else {
        timeheap_change(&h->th, &sum);
        return 0;
    }
}

void
evtimer_del(struct timeout *h)
{
    if (h->th.i >= 0) {
        timeheap_remove(&h->th);
        h->th.i = -1;
    }
}

void
evtimers_run(void)
{
    struct timespec now;
    evtimer_gettime(&now);
    while (timeheap_size() > 0) {
        struct timespec diff = subtime(timeheap_top(), now);
        if (diff.tv_sec < 0) {
            struct timeout *t = timeheap_remove_top();
            t->th.i = -1;
            t->cb(-1, EV_TIMEOUT, t->arg);
        } else
            break;
    }
}

struct timespec
evtimer_delay(void)
{
    struct timespec now, diff;
    if (timeheap_size() == 0) {
        diff.tv_sec = -1;
        diff.tv_nsec = 0;
    } else {
        evtimer_gettime(&now);
        diff = subtime(timeheap_top(), now);
        if (diff.tv_sec < 0) {
            diff.tv_sec = 0;
            diff.tv_nsec = 0;
        }
    }
    return diff;
}
