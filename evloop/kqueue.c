#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "evloop.h"

static int m_kq;

static struct kevent m_evs[100];
static uint8_t m_valid[100];

int
evloop_init(void)
{
    if (timeheap_init() != 0)
        return -1;
    m_kq = kqueue();
    return m_kq >= 0 ? 0 : -1;
}

int
fdev_new(struct fdev *ev, int fd, uint16_t flags, evloop_cb_t cb, void *arg)
{
    ev->fd = fd;
    ev->cb = cb;
    ev->arg = arg;
    ev->flags = 0;
    ev->index = -1;
    return fdev_enable(ev, flags);
}

int
fdev_enable(struct fdev *ev, uint16_t flags)
{
    struct kevent kev[2], *kp = NULL;
    int count = 0;
    uint16_t sf = ev->flags;
    ev->flags |= flags;
    if ((sf & EV_READ) == 0 && (flags & EV_READ) != 0) {
        EV_SET(&kev[0], ev->fd, EVFILT_READ, EV_ADD, 0, 0, ev);
        kp = kev;
        count = 1;
    }
    if ((sf & EV_WRITE) == 0 && (flags & EV_WRITE) != 0) {
        EV_SET(&kev[1], ev->fd, EVFILT_WRITE, EV_ADD, 0, 0, ev);
        if (count == 0)
            kp = &kev[1];
        count++;
    }
    return count > 0 ? kevent(m_kq, kp, count, NULL, 0, NULL) : 0;
}

int
fdev_disable(struct fdev *ev, uint16_t flags)
{
    struct kevent kev[2], *kp = NULL;
    int count = 0;
    uint16_t sf = ev->flags;
    ev->flags &= ~flags;
    if ((sf & EV_READ) != 0 && (flags & EV_READ) != 0) {
        EV_SET(&kev[0], ev->fd, EVFILT_READ, EV_DELETE, 0, 0, ev);
        kp = kev;
        count = 1;
    }
    if ((sf & EV_WRITE) != 0 && (flags & EV_WRITE) != 0) {
        EV_SET(&kev[1], ev->fd, EVFILT_WRITE, EV_DELETE, 0, 0, ev);
        if (count == 0)
            kp = &kev[1];
        count++;
    }
    return count > 0 ? kevent(m_kq, kp, count, NULL, 0, NULL) : 0;
}

int
fdev_del(struct fdev *ev)
{
    if (ev->index >= 0)
        m_valid[ev->index] = 0;
    return fdev_disable(ev, EV_READ|EV_WRITE);
}

int
evloop(void)
{
    int nev, i;
    struct timespec delay;
    while (1) {
        timers_run();
        delay = timer_delay();

        if ((nev = kevent(m_kq, NULL, 0, m_evs, 100, &delay)) < 0) {
            if (errno == EINTR)
                continue;
            else
                return -1;
        }
        memset(m_valid, 1, nev);
        for (i = 0; i < nev; i++) {
            struct fdev *ev = (struct fdev *)m_evs[i].udata;
            ev->index = i;
        }
        for (i = 0; i < nev; i++) {
            if (m_evs[i].flags & EV_ERROR) {
                errno = m_evs[i].data;
                return -1;
            }
            struct fdev *ev = (struct fdev *)m_evs[i].udata;
            if (m_valid[i] && ev->flags & EV_READ &&
                m_evs[i].filter == EVFILT_READ)
                ev->cb(ev->fd, EV_READ, ev->arg);
            if (m_valid[i] && ev->flags & EV_WRITE &&
                m_evs[i].filter == EVFILT_WRITE)
                ev->cb(ev->fd, EV_WRITE, ev->arg);
            if (m_valid[i])
                ev->index = -1;
        }
    }
}
