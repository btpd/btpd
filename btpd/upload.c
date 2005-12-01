
#include "btpd.h"

static struct event m_choke_timer;
static unsigned m_npeers;
static struct peer_tq m_peerq = BTPDQ_HEAD_INITIALIZER(m_peerq);

static void
choke_do(void)
{
    struct peer *p;
    BTPDQ_FOREACH(p, &m_peerq, ul_entry)
        if (p->flags & PF_I_CHOKE)
            peer_unchoke(p);
}

static void
choke_cb(int sd, short type, void *arg)
{
    evtimer_add(&m_choke_timer, (& (struct timeval) { 10, 0}));
    choke_do();
}

void
ul_on_new_peer(struct peer *p)
{
    m_npeers++;
    BTPDQ_INSERT_HEAD(&m_peerq, p, ul_entry);
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
ul_on_lost_torrent(struct torrent *tp)
{
    struct peer *p = BTPDQ_FIRST(&m_peerq);
    while (p != NULL) {
        struct peer *next = BTPDQ_NEXT(p, p_entry);
        BTPDQ_REMOVE(&m_peerq, p, ul_entry);
        m_npeers--;
        p = next;
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
    evtimer_set(&m_choke_timer, choke_cb, NULL);
    evtimer_add(&m_choke_timer, (& (struct timeval) { 10, 0 }));
}
