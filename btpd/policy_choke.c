#include "btpd.h"

static int
rate_cmp(long rate1, long rate2)
{
    if (rate1 < rate2)
	return -1;
    else if (rate1 == rate2)
	return 0;
    else
	return 1;
}

static int
dwnrate_cmp(const void *p1, const void *p2)
{
    long rate1 = (*(struct peer **)p1)->rate_dwn;
    long rate2 = (*(struct peer **)p2)->rate_dwn;
    return rate_cmp(rate1, rate2);
}

static int
uprate_cmp(const void *p1, const void *p2)
{
    long rate1 = (*(struct peer **)p1)->rate_up;
    long rate2 = (*(struct peer **)p2)->rate_up;
    return rate_cmp(rate1, rate2);
}

void
choke_alg(struct torrent *tp)
{
    assert(tp->npeers > 0);

    int i;
    struct peer *p;
    struct peer *psort[tp->npeers];

    i = 0;
    BTPDQ_FOREACH(p, &tp->peers, p_entry)
	psort[i++] = p;
    
    if (tp->have_npieces == tp->meta.npieces)
	qsort(psort, tp->npeers, sizeof(p), uprate_cmp);
    else
	qsort(psort, tp->npeers, sizeof(p), dwnrate_cmp);
    
    tp->ndown = 0;
    if (tp->optimistic != NULL) {
	if (tp->optimistic->flags & PF_I_CHOKE)
	    peer_unchoke(tp->optimistic);
	if (tp->optimistic->flags & PF_P_WANT)
	    tp->ndown = 1;
    }

    for (i = tp->npeers - 1; i >= 0; i--) {
	if (psort[i] == tp->optimistic)
	    continue;
	if (tp->ndown < 4) {
	    if (psort[i]->flags & PF_P_WANT)
		tp->ndown++;
	    if (psort[i]->flags & PF_I_CHOKE)
		peer_unchoke(psort[i]);
	} else {
	    if ((psort[i]->flags & PF_I_CHOKE) == 0)
		peer_choke(psort[i]);
	}
    }

    tp->choke_time = btpd_seconds + 10;
}

void
next_optimistic(struct torrent *tp, struct peer *np)
{
    if (np != NULL)
	tp->optimistic = np;
    else if (tp->optimistic == NULL)
	tp->optimistic = BTPDQ_FIRST(&tp->peers);
    else {
	np = BTPDQ_NEXT(tp->optimistic, p_entry);
	if (np != NULL)
	    tp->optimistic = np;
	else
	    tp->optimistic = BTPDQ_FIRST(&tp->peers);
    }
    assert(tp->optimistic != NULL);
    choke_alg(tp);
    tp->opt_time = btpd_seconds + 30;
}
