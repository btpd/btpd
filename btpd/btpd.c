#include "btpd.h"

#include <openssl/sha.h>
#include <signal.h>

static uint8_t m_peer_id[20];
static struct timeout m_heartbeat;
static int m_signal;
static int m_shutdown;
static int m_ghost;

long btpd_seconds;

void
btpd_exit(int code)
{
    btpd_log(BTPD_L_BTPD, "Exiting.\n");
    exit(code);
}

extern int pidfd;
void ipc_shutdown(void);
void net_shutdown(void);

static void
death_procedure(void)
{
    assert(m_shutdown);
    if (torrent_count() == 0)
        btpd_exit(0);
    if (!m_ghost && torrent_count() == torrent_ghosts()) {
        btpd_log(BTPD_L_BTPD, "Entering pre exit mode. Bye!\n");
        fclose(stderr);
        fclose(stdout);
        net_shutdown();
        ipc_shutdown();
        close(pidfd);
        m_ghost = 1;
    }
}

void
btpd_shutdown(void)
{
    m_shutdown = 1;
    struct torrent *tp, *next;
    BTPDQ_FOREACH_MUTABLE(tp, torrent_get_all(), entry, next)
        torrent_stop(tp, 0);
    death_procedure();
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
signal_handler(int signal)
{
    m_signal = signal;
}

static void
heartbeat_cb(int fd, short type, void *arg)
{
    btpd_timer_add(&m_heartbeat, (& (struct timespec) { 1, 0 }));
    btpd_seconds++;
    net_on_tick();
    torrent_on_tick_all();
    if (m_signal) {
        btpd_log(BTPD_L_BTPD, "Got signal %d.\n", m_signal);
        m_signal = 0;
        if (!m_shutdown)
            btpd_shutdown();
    }
    if (m_shutdown)
        death_procedure();
}

void tr_init(void);
void ipc_init(void);
void td_init(void);
void addrinfo_init(void);

void
btpd_init(void)
{
    struct sigaction sa;
    unsigned long seed;
    uint8_t idcon[1024];
    struct timeval now;
    int n;

    bzero(&sa, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

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
    addrinfo_init();
    net_init();
    ipc_init();
    ul_init();
    cm_init();
    tr_init();
    tlib_init();

    timer_init(&m_heartbeat, heartbeat_cb, NULL);
    btpd_timer_add(&m_heartbeat, (& (struct timespec) { 1, 0 }));
}
