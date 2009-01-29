#include "btpd.h"

#define CHOKE_INTERVAL (& (struct timespec) { 10, 0 })

static struct timeout m_choke_timer;
static unsigned m_npeers;
static struct peer_tq m_peerq = BTPDQ_HEAD_INITIALIZER(m_peerq);
static int m_max_uploads;

struct peer_sort {
    struct peer *p;
    unsigned i;
};

static int
rate_cmp(const void *arg1, const void *arg2)
{
    struct peer *p1 = ((struct peer_sort *)arg1)->p;
    struct peer *p2 = ((struct peer_sort *)arg2)->p;
    unsigned long rate1 = cm_full(p1->n->tp) ? p1->rate_up / 2: p1->rate_dwn;
    unsigned long rate2 = cm_full(p2->n->tp) ? p2->rate_up / 2: p2->rate_dwn;
    if (rate1 < rate2)
        return -1;
    else if (rate1 == rate2)
        return 0;
    else
        return 1;
}

static void
choke_do(void)
{
    if (m_max_uploads < 0) {
        struct peer *p;
        BTPDQ_FOREACH(p, &m_peerq, ul_entry)
            if (p->flags & PF_I_CHOKE)
                peer_unchoke(p);
    } else if (m_max_uploads == 0) {
        struct peer *p;
        BTPDQ_FOREACH(p, &m_peerq, ul_entry)
            if ((p->flags & PF_I_CHOKE) == 0)
                peer_choke(p);
    } else {
        struct peer_sort worthy[m_npeers];
        int nworthy = 0;
        int i = 0;
        int found = 0;
        struct peer *p;
        int unchoked[m_npeers];

        BTPDQ_FOREACH(p, &m_peerq, ul_entry) {
            int ok = 0;
            if (!peer_full(p)) {
                if (cm_full(p->n->tp)) {
                    if (p->rate_up > 0)
                        ok = 1;
                } else if (peer_active_down(p) && p->rate_dwn > 0)
                    ok = 1;
            }
            if (ok) {
                worthy[nworthy].p = p;
                worthy[nworthy].i = i;
                nworthy++;
            }
            i++;
        }
        qsort(worthy, nworthy, sizeof(worthy[0]), rate_cmp);

        bzero(unchoked, sizeof(unchoked));
        for (i = nworthy - 1; i >= 0 && found < m_max_uploads - 1; i--) {
            if ((worthy[i].p->flags & PF_P_WANT) != 0)
                found++;
            if ((worthy[i].p->flags & PF_I_CHOKE) != 0)
                peer_unchoke(worthy[i].p);
            unchoked[worthy[i].i] = 1;
        }

        i = 0;
        BTPDQ_FOREACH(p, &m_peerq, ul_entry) {
            if (!unchoked[i]) {
                if (found < m_max_uploads && !peer_full(p)) {
                    if (p->flags & PF_P_WANT)
                        found++;
                    if (p->flags & PF_I_CHOKE)
                        peer_unchoke(p);
                } else {
                    if ((p->flags & PF_I_CHOKE) == 0)
                        peer_choke(p);
                }
            }
            i++;
        }
    }
}

static void
shuffle_optimists(void)
{
    for (int i = 0; i < m_npeers; i++) {
        struct peer *p = BTPDQ_FIRST(&m_peerq);
        if ((p->flags & (PF_P_WANT|PF_I_CHOKE)) == (PF_P_WANT|PF_I_CHOKE)) {
            break;
        } else {
            BTPDQ_REMOVE(&m_peerq, p, ul_entry);
            BTPDQ_INSERT_TAIL(&m_peerq, p, ul_entry);
        }
    }
}

static void
choke_cb(int sd, short type, void *arg)
{
    btpd_timer_add(&m_choke_timer, CHOKE_INTERVAL);
    static int cb_count = 0;
    cb_count++;
    if (cb_count % 3 == 0)
        shuffle_optimists();
    choke_do();
}

void
ul_on_new_peer(struct peer *p)
{
    long where = rand_between(-2, m_npeers);
    if (where < 1)
        BTPDQ_INSERT_HEAD(&m_peerq, p, ul_entry);
    else {
        struct peer *it = BTPDQ_FIRST(&m_peerq);
        where--;
        while (where > 0) {
            it = BTPDQ_NEXT(it, ul_entry);
            where--;
        }
        BTPDQ_INSERT_AFTER(&m_peerq, it, p, ul_entry);
    }
    m_npeers++;
    choke_do();
}

void
ul_on_lost_peer(struct peer *p)
{
    assert(m_npeers > 0);
    BTPDQ_REMOVE(&m_peerq, p, ul_entry);
    m_npeers--;
    if ((p->flags & (PF_P_WANT|PF_I_CHOKE)) == PF_P_WANT)
        choke_do();
}

void
ul_on_lost_torrent(struct net *n)
{
    struct peer *p;
    BTPDQ_FOREACH(p, &n->peers, p_entry) {
        BTPDQ_REMOVE(&m_peerq, p, ul_entry);
        m_npeers--;
    }
    choke_do();
}

void
ul_on_interest(struct peer *p)
{
    if ((p->flags & PF_I_CHOKE) == 0)
        choke_do();
}

void
ul_on_uninterest(struct peer *p)
{
    if ((p->flags & PF_I_CHOKE) == 0)
        choke_do();
}

void
ul_init(void)
{
    if (net_max_uploads >= -1)
        m_max_uploads = net_max_uploads;
    else {
        if (net_bw_limit_out == 0)
            m_max_uploads = 8;
        else if (net_bw_limit_out < (10 << 10))
            m_max_uploads = 2;
        else if (net_bw_limit_out < (20 << 10))
            m_max_uploads = 3;
        else if (net_bw_limit_out < (40 << 10))
            m_max_uploads = 4;
        else
            m_max_uploads = 5 + (net_bw_limit_out / (100 << 10));
    }

    evtimer_init(&m_choke_timer, choke_cb, NULL);
    btpd_timer_add(&m_choke_timer, CHOKE_INTERVAL);
}
