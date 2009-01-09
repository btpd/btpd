#include <sys/epoll.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "evloop.h"

static int m_epfd;

static struct epoll_event m_evs[100];
static uint8_t m_valid[100];

int
evloop_init(void)
{
    if (timeheap_init() != 0)
        return -1;
    m_epfd = epoll_create(getdtablesize());
    return m_epfd >= 0 ? 0 : -1;
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
    struct epoll_event epev;
    int err = 0;
    uint16_t sf = ev->flags;
    ev->flags |= flags;
    if (sf != ev->flags) {
        epev.data.ptr = ev;
        epev.events =
            ((ev->flags & EV_READ) ? EPOLLIN : 0) |
            ((ev->flags & EV_WRITE) ? EPOLLOUT : 0);
        if (sf == 0)
            err = epoll_ctl(m_epfd, EPOLL_CTL_ADD, ev->fd, &epev);
        else
            err = epoll_ctl(m_epfd, EPOLL_CTL_MOD, ev->fd, &epev);
    }
    return err;
}

int
fdev_disable(struct fdev *ev, uint16_t flags)
{
    struct epoll_event epev;
    int err = 0;
    uint16_t sf = ev->flags;
    ev->flags &= ~flags;
    if (sf != ev->flags) {
        epev.data.ptr = ev;
        epev.events =
            ((ev->flags & EV_READ) ? EPOLLIN : 0) |
            ((ev->flags & EV_WRITE) ? EPOLLOUT : 0);
        if (ev->flags == 0)
            err = epoll_ctl(m_epfd, EPOLL_CTL_DEL, ev->fd, &epev);
        else
            err = epoll_ctl(m_epfd, EPOLL_CTL_MOD, ev->fd, &epev);
    }
    return err;
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
    int nev, i, millisecs;
    struct timespec delay;
    while (1) {
        timers_run();
        delay = timer_delay();
        if (delay.tv_sec >= 0)
            millisecs = delay.tv_sec * 1000 + delay.tv_nsec / 1000000;
        else
            millisecs = -1;

        if ((nev = epoll_wait(m_epfd, m_evs, 100, millisecs)) < 0) {
            if (errno == EINTR)
                continue;
            else
                return -1;
        }
        memset(m_valid, 1, nev);
        for (i = 0; i < nev; i++) {
            struct fdev *ev = m_evs[i].data.ptr;
            ev->index = i;
        }
        for (i = 0; i < nev; i++) {
            struct fdev *ev = m_evs[i].data.ptr;
            if ((m_valid[i] &&
                    ev->flags & EV_READ &&
                    m_evs[i].events & (EPOLLIN|EPOLLERR|EPOLLHUP)))
                ev->cb(ev->fd, EV_READ, ev->arg);
            if ((m_valid[i] && ev->flags & EV_WRITE &&
                    m_evs[i].events & (EPOLLOUT|EPOLLERR|EPOLLHUP)))
                ev->cb(ev->fd, EV_WRITE, ev->arg);
            if (m_valid[i])
                ev->index = -1;
        }
    }
}
