#include "btpd.h"

#include <openssl/sha.h>
#include <signal.h>

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
        torrent_stop(tp, 0);
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
                torrent_stop(tp, 0);
        if (grace_seconds >= 0) {
            if (event_once(-1, EV_TIMEOUT, grace_cb, NULL,
                    (& (struct timeval) { grace_seconds, 0 })) != 0)
                btpd_err("failed to add event (%s).\n", strerror(errno));
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
    torrent_on_tick_all();
    if (m_shutdown && torrent_count() == 0)
        btpd_exit(0);
}

void tr_init(void);
void ipc_init(void);
void td_init(void);

void
btpd_init(void)
{
    unsigned long seed;
    uint8_t idcon[1024];
    struct timeval now;
    int n;

    gettimeofday(&now, NULL);
    n = snprintf(idcon, sizeof(idcon), "%ld%ld%d", now.tv_sec, now.tv_usec,
        net_port);
    if (n < sizeof(idcon))
        gethostname(idcon + n, sizeof(idcon) - n);
    idcon[sizeof(idcon) - 1] = '\0';
    n = strlen(idcon);

    SHA1(idcon, n, m_peer_id);
    bcopy(m_peer_id, &seed, sizeof(seed));
    bcopy(BTPD_VERSION, m_peer_id, sizeof(BTPD_VERSION) - 1);
    m_peer_id[sizeof(BTPD_VERSION) - 1] = '|';

    srandom(seed);

    td_init();
    net_init();
    ipc_init();
    ul_init();
    cm_init();
    tr_init();
    tlib_init();

    signal(SIGPIPE, SIG_IGN);

    signal_set(&m_sigint, SIGINT, signal_cb, NULL);
    btpd_ev_add(&m_sigint, NULL);
    signal_set(&m_sigterm, SIGTERM, signal_cb, NULL);
    btpd_ev_add(&m_sigterm, NULL);
    evtimer_set(&m_heartbeat, heartbeat_cb, NULL);
    btpd_ev_add(&m_heartbeat, (& (struct timeval) { 1, 0 }));

    if (!empty_start)
        active_start();
    else
        active_clear();
}
