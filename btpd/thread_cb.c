#include <sys/types.h>

#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "btpd.h"

struct td_cb {
    void (*cb)(void *);
    void *arg;
    BTPDQ_ENTRY(td_cb) entry;
};

BTPDQ_HEAD(td_cb_tq, td_cb);

static int m_td_rd, m_td_wr;
static struct event m_td_ev;
static struct td_cb_tq m_td_cbs = BTPDQ_HEAD_INITIALIZER(m_td_cbs);
static pthread_mutex_t m_td_lock;

void
td_acquire_lock(void)
{
    pthread_mutex_lock(&m_td_lock);
}

void
td_release_lock(void)
{
    pthread_mutex_unlock(&m_td_lock);
}

void
td_post(void (*fun)(void *), void *arg)
{
    struct td_cb *cb = btpd_calloc(1, sizeof(*cb));
    cb->cb = fun;
    cb->arg = arg;
    BTPDQ_INSERT_TAIL(&m_td_cbs, cb, entry);
}

void
td_post_end(void)
{
    char c = '1';
    td_release_lock();
    write(m_td_wr, &c, sizeof(c));
}

static void
td_cb(int fd, short type, void *arg)
{
    char buf[1024];
    struct td_cb_tq tmpq =  BTPDQ_HEAD_INITIALIZER(tmpq);
    struct td_cb *cb, *next;

    read(fd, buf, sizeof(buf));
    td_acquire_lock();
    BTPDQ_FOREACH_MUTABLE(cb, &m_td_cbs, entry, next)
        BTPDQ_INSERT_TAIL(&tmpq, cb, entry);
    BTPDQ_INIT(&m_td_cbs);
    td_release_lock();

    BTPDQ_FOREACH_MUTABLE(cb, &tmpq, entry, next) {
        cb->cb(cb->arg);
        free(cb);
    }
}

void
td_init(void)
{
    int err;
    int fds[2];
    if (pipe(fds) == -1) {
        btpd_err("Couldn't create thread callback pipe (%s).\n",
            strerror(errno));
    }
    m_td_rd = fds[0];
    m_td_wr = fds[1];
    if ((err = pthread_mutex_init(&m_td_lock, NULL)) != 0)
        btpd_err("Couldn't create mutex (%s).\n", strerror(err));

    event_set(&m_td_ev, m_td_rd, EV_READ|EV_PERSIST, td_cb, NULL);
    btpd_ev_add(&m_td_ev, NULL);
}
