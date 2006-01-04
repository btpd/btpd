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
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "btpd.h"

struct child {
    pid_t pid;
    void *arg;
    void (*cb)(pid_t, void *);
    BTPDQ_ENTRY(child) entry;
};

BTPDQ_HEAD(child_tq, child);

static uint8_t m_peer_id[20];
static struct event m_sigint;
static struct event m_sigterm;
static struct event m_sigchld;
static struct child_tq m_kids = BTPDQ_HEAD_INITIALIZER(m_kids);
static unsigned m_ntorrents;
static struct torrent_tq m_torrents = BTPDQ_HEAD_INITIALIZER(m_torrents);

void
btpd_shutdown(void)
{
    struct torrent *tp;

    tp = BTPDQ_FIRST(&m_torrents);
    while (tp != NULL) {
        struct torrent *next = BTPDQ_NEXT(tp, entry);
        torrent_deactivate(tp);
        tp = next;
    }
    btpd_log(BTPD_L_BTPD, "Exiting.\n");
    exit(0);
}

static void
signal_cb(int signal, short type, void *arg)
{
    btpd_log(BTPD_L_BTPD, "Got signal %d.\n", signal);
    btpd_shutdown();
}

void
btpd_add_child(pid_t pid, void (*cb)(pid_t, void *), void *arg)
{
    struct child *kid = btpd_calloc(1, sizeof(*kid));
    kid->pid = pid;
    kid->arg = arg;
    kid->cb = cb;
    BTPDQ_INSERT_TAIL(&m_kids, kid, entry);
}

static void
child_cb(int signal, short type, void *arg)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            struct child *kid = BTPDQ_FIRST(&m_kids);
            while (kid != NULL && kid->pid != pid)
                kid = BTPDQ_NEXT(kid, entry);
            assert(kid != NULL);
            BTPDQ_REMOVE(&m_kids, kid, entry);
            kid->cb(kid->pid, kid->arg);
            free(kid);
        }
    }
}

void
btpd_add_torrent(struct torrent *tp)
{
    BTPDQ_INSERT_TAIL(&m_torrents, tp, entry);
    m_ntorrents++;
}

void
btpd_del_torrent(struct torrent *tp)
{
    BTPDQ_REMOVE(&m_torrents, tp, entry);
    m_ntorrents--;
}

const struct torrent_tq *
btpd_get_torrents(void)
{
    return &m_torrents;
}

unsigned
btpd_get_ntorrents(void)
{
    return m_ntorrents;
}

struct torrent *
btpd_get_torrent(const uint8_t *hash)
{
    struct torrent *tp = BTPDQ_FIRST(&m_torrents);
    while (tp != NULL && bcmp(hash, tp->meta.info_hash, 20) != 0)
        tp = BTPDQ_NEXT(tp, entry);
    return tp;
}

const uint8_t *
btpd_get_peer_id(void)
{
    return m_peer_id;
}

static int
nodot(struct dirent *dp)
{
    return !(strcmp(".", dp->d_name) == 0 || strcmp("..", dp->d_name) == 0);
}

static void
load_library(void)
{
    int ne;
    struct dirent **entries;
    if ((ne = scandir("library", &entries, nodot, NULL)) < 0)
        btpd_err("Couldn't open the library.\n");

    for (int i = 0; i < ne; i++) {
        struct torrent *tp;
        struct dirent *e = entries[i];
        if (torrent_create(&tp, e->d_name) == 0)
            btpd_add_torrent(tp);
        free(e);
    }
    free(entries);
}

extern void ipc_init(void);

void
btpd_init(void)
{
    bcopy(BTPD_VERSION, m_peer_id, sizeof(BTPD_VERSION) - 1);
    m_peer_id[sizeof(BTPD_VERSION) - 1] = '|';
    srandom(time(NULL));
    for (int i = sizeof(BTPD_VERSION); i < 20; i++)
        m_peer_id[i] = rand_between(0, 255);

    net_init();
    //ipc_init();
    ul_init();

    load_library();

    struct torrent *tp;
    BTPDQ_FOREACH(tp, &m_torrents, entry)
        torrent_activate(tp);

    signal(SIGPIPE, SIG_IGN);

    signal_set(&m_sigint, SIGINT, signal_cb, NULL);
    signal_add(&m_sigint, NULL);
    signal_set(&m_sigterm, SIGTERM, signal_cb, NULL);
    signal_add(&m_sigterm, NULL);
    signal_set(&m_sigchld, SIGCHLD, child_cb, NULL);
    signal_add(&m_sigchld, NULL);
}
