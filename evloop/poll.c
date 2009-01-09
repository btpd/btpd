#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "evloop.h"

#define POLL_INIT_SIZE 64

struct poll_ev {
    struct fdev *ev;
    evloop_cb_t cb;
    void *arg;
};

static struct pollfd *m_pfds;
static struct poll_ev *m_pevs;

static int m_cap, m_size;
static int m_cur = -1, m_curdel;

static int
poll_grow(void)
{
    int ncap = m_cap * 2;
    struct pollfd *nm_pfds = realloc(m_pfds, ncap * sizeof(*m_pfds));
    struct poll_ev *nm_pevs = realloc(m_pevs, ncap * sizeof(*m_pevs));
    if (nm_pfds != NULL)
        m_pfds = nm_pfds;
    if (nm_pevs != NULL)
        m_pevs = nm_pevs;
    if (nm_pfds == NULL || nm_pevs == NULL)
        return errno;
    m_cap = ncap;
    return 0;
}

int
evloop_init(void)
{
    if (timeheap_init() != 0)
        return -1;
    m_cap = POLL_INIT_SIZE;
    m_size = 0;
    if ((m_pfds = calloc(m_cap, sizeof(*m_pfds))) == NULL)
        return -1;
    if ((m_pevs = calloc(m_cap, sizeof(*m_pevs))) == NULL) {
        free(m_pfds);
        return -1;
    }
    return 0;
}

int
fdev_new(struct fdev *ev, int fd, uint16_t flags, evloop_cb_t cb, void *arg)
{
    if (m_size == m_cap && poll_grow() != 0)
        return errno;
    ev->i = m_size;
    m_size++;
    m_pfds[ev->i].fd = fd;
    m_pfds[ev->i].events =
        ((flags & EV_READ) ? POLLIN : 0) |
        ((flags & EV_WRITE) ? POLLOUT : 0);
    m_pevs[ev->i].ev = ev;
    m_pevs[ev->i].cb = cb;
    m_pevs[ev->i].arg = arg;
    return 0;
}

int
fdev_enable(struct fdev *ev, uint16_t flags)
{
    m_pfds[ev->i].events |= 
        ((flags & EV_READ) ? POLLIN : 0) |
        ((flags & EV_WRITE) ? POLLOUT : 0);
    return 0;
}

int
fdev_disable(struct fdev *ev, uint16_t flags)
{
    short pflags =
        ((flags & EV_READ) ? POLLIN : 0) |
        ((flags & EV_WRITE) ? POLLOUT : 0);
    m_pfds[ev->i].events &= ~pflags;
    return 0;
}

int
fdev_del(struct fdev *ev)
{
    assert(ev->i < m_size);
    m_size--;
    m_pfds[ev->i] = m_pfds[m_size];
    m_pevs[ev->i] = m_pevs[m_size];
    m_pevs[ev->i].ev->i = ev->i;
    if (ev->i == m_cur)
        m_curdel = 1;
    return 0;
}

int
evloop(void)
{
    int millisecs;
    struct timespec delay;
    while (1) {
        timers_run();

        delay = timer_delay();
        if (delay.tv_sec >= 0)
            millisecs = delay.tv_sec * 1000 + delay.tv_nsec / 1000000;
        else
            millisecs = -1;

        if (poll(m_pfds, m_size, millisecs) < 0) {
            if (errno == EINTR)
                continue;
            else
                return -1;
        }

        m_cur = 0;
        while (m_cur < m_size) {
            struct pollfd *pfd = &m_pfds[m_cur];
            struct poll_ev *pev = &m_pevs[m_cur];
            if ((pfd->events & POLLIN &&
                    pfd->revents & (POLLIN|POLLERR|POLLHUP)))
                pev->cb(pfd->fd, EV_READ, pev->arg);
            if ((!m_curdel && pfd->events & POLLOUT &&
                    pfd->revents & (POLLOUT|POLLERR|POLLHUP)))
                pev->cb(pfd->fd, EV_WRITE, pev->arg);
            if (!m_curdel)
                m_cur++;
            else
                m_curdel = 0;
        }
        m_cur = -1;
    }
}
