#include <sys/types.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <locale.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "btpd.h"
#include "http.h"

static uint8_t m_peer_id[20];
static struct event m_sigint;
static struct event m_sigterm;
static struct event m_heartbeat;
static int m_shutdown;

long btpd_seconds;

void
btpd_exit(int code)
{
    btpd_log(BTPD_L_BTPD, "Exiting.\n");
    exit(code);
}

static void
grace_cb(int fd, short type, void *arg)
{
    struct torrent *tp;
    BTPDQ_FOREACH(tp, torrent_get_all(), entry)
        torrent_stop(tp);
}

void
btpd_shutdown(int grace_seconds)
{
    if (torrent_count() == 0)
        btpd_exit(0);
    else {
        struct torrent *tp;
        m_shutdown = 1;
        BTPDQ_FOREACH(tp, torrent_get_all(), entry)
            if (tp->state != T_STOPPING)
                torrent_stop(tp);
        if (grace_seconds >= 0) {
            event_once(-1, EV_TIMEOUT, grace_cb, NULL,
                (& (struct timeval) { grace_seconds, 0 }));
        }
    }
}

int btpd_is_stopping(void)
{
    return m_shutdown;
}

const uint8_t *
btpd_get_peer_id(void)
{
    return m_peer_id;
}

void
btpd_on_no_torrents(void)
{
    if (m_shutdown)
        btpd_exit(0);
}

static void
signal_cb(int signal, short type, void *arg)
{
    btpd_log(BTPD_L_BTPD, "Got signal %d.\n", signal);
    btpd_shutdown(30);
}

static void
heartbeat_cb(int fd, short type, void *arg)
{
    btpd_ev_add(&m_heartbeat, (& (struct timeval) { 1, 0 }));
    btpd_seconds++;
    net_on_tick();
}

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

static void
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

void ipc_init(void);

void
btpd_init(void)
{
    bcopy(BTPD_VERSION, m_peer_id, sizeof(BTPD_VERSION) - 1);
    m_peer_id[sizeof(BTPD_VERSION) - 1] = '|';
    srandom(time(NULL));
    for (int i = sizeof(BTPD_VERSION); i < 20; i++)
        m_peer_id[i] = rand_between(0, 255);

    td_init();
    http_init();
    net_init();
    ipc_init();
    ul_init();
    cm_init();

    signal(SIGPIPE, SIG_IGN);

    signal_set(&m_sigint, SIGINT, signal_cb, NULL);
    btpd_ev_add(&m_sigint, NULL);
    signal_set(&m_sigterm, SIGTERM, signal_cb, NULL);
    btpd_ev_add(&m_sigterm, NULL);
    evtimer_set(&m_heartbeat, heartbeat_cb, NULL);
    btpd_ev_add(&m_heartbeat, (& (struct timeval) { 1, 0 }));
}
