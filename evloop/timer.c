#include <time.h>

#include "evloop.h"
#include "timeheap.h"

#if defined(CLOCK_MONOTONIC_FAST)
#define TIMER_CLOCK CLOCK_MONOTONIC_FAST
#elif defined(CLOCK_MONOTONIC)
#define TIMER_CLOCK CLOCK_MONOTONIC
#else
#error CLOCK_MONOTONIC needed!
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
    clock_gettime(TIMER_CLOCK, &now);
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
    clock_gettime(TIMER_CLOCK, &now);
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
        clock_gettime(TIMER_CLOCK, &now);
        diff = subtime(timeheap_top(), now);
        if (diff.tv_sec < 0) {
            diff.tv_sec = 0;
            diff.tv_nsec = 0;
        }
    }
    return diff;
}
