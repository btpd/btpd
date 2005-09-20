#include <sys/types.h>
#include <sys/mman.h>

#include "btpd.h"
#include "tracker_req.h"

void
cm_by_second(struct torrent *tp)
{
    if (btpd.seconds == tp->tracker_time)
	tracker_req(tp, TR_EMPTY);

    if (btpd.seconds == tp->opt_time)
	next_optimistic(tp, NULL);

    if (btpd.seconds == tp->choke_time)
	choke_alg(tp);

    struct peer *p;
    int ri = btpd.seconds % RATEHISTORY;

    BTPDQ_FOREACH(p, &tp->peers, cm_entry) {
	p->rate_to_me[ri] = 0;
	p->rate_from_me[ri] = 0;
    }
}

/*
 * Called when a peer announces it's got a new piece.
 *
 * If the piece is missing or unfull we increase the peer's
 * wanted level and if possible call cm_on_download.
 */
void
cm_on_piece_ann(struct peer *p, uint32_t index)
{
    struct torrent *tp = p->tp;
    tp->piece_count[index]++;
    if (has_bit(tp->piece_field, index))
	return;
    struct piece *pc = cm_find_piece(tp, index);
    if (tp->endgame) {
	assert(pc != NULL);
	peer_want(p, index);
	if (!peer_chokes(p) && !peer_laden(p))
	    cm_assign_requests_eg(p);
    } else if (pc == NULL) {
	peer_want(p, index);
	if (!peer_chokes(p) && !peer_laden(p)) {
	    pc = cm_new_piece(tp, index);
	    if (pc != NULL)
		cm_piece_assign_requests(pc, p);
	}
    } else if (!piece_full(pc)) {
	peer_want(p, index);
	if (!peer_chokes(p) && !peer_laden(p))
	    cm_piece_assign_requests(pc, p);
    }
}

void
cm_on_download(struct peer *p)
{
    assert(peer_wanted(p));
    struct torrent *tp = p->tp;
    if (tp->endgame) {
	cm_assign_requests_eg(p);
    } else {
	unsigned count = cm_assign_requests(p);
	if (count == 0 && !p->tp->endgame) // We may have entered end game.
	    assert(!peer_wanted(p) || peer_laden(p));
    }
}

void
cm_on_unchoke(struct peer *p)
{
    if (peer_wanted(p))
	cm_on_download(p);
}

void
cm_on_undownload(struct peer *p)
{
    if (!p->tp->endgame)
	cm_unassign_requests(p);
    else
	cm_unassign_requests_eg(p);
}

void
cm_on_choke(struct peer *p)
{
    if (p->nreqs_out > 0)
	cm_on_undownload(p);
}

void
cm_on_upload(struct peer *p)
{
    choke_alg(p->tp);
}

void
cm_on_interest(struct peer *p)
{
    if ((p->flags & PF_I_CHOKE) == 0)
	cm_on_upload(p);
}

void
cm_on_unupload(struct peer *p)
{
    choke_alg(p->tp);
}

void
cm_on_uninterest(struct peer *p)
{
    if ((p->flags & PF_I_CHOKE) == 0)
	cm_on_unupload(p);
}

/**
 * Called when a piece has been tested positively.
 */
void
cm_on_ok_piece(struct piece *pc)
{
    struct peer *p;
    struct torrent *tp = pc->tp;

    btpd_log(BTPD_L_POL, "Got piece: %u.\n", pc->index);

    set_bit(tp->piece_field, pc->index);
    tp->have_npieces++;
    msync(tp->imem, tp->isiz, MS_ASYNC);

    struct net_buf *have = nb_create_have(pc->index);
    BTPDQ_FOREACH(p, &tp->peers, cm_entry)
	peer_send(p, have);

    if (tp->endgame)
	BTPDQ_FOREACH(p, &tp->peers, cm_entry)
	    if (peer_has(p, pc->index))
		peer_unwant(p, pc->index);

    assert(pc->nreqs == 0);
    piece_free(pc);

    if (torrent_has_all(tp)) {
	btpd_log(BTPD_L_BTPD, "Finished: %s.\n", tp->relpath);
	tracker_req(tp, TR_COMPLETED);
	BTPDQ_FOREACH(p, &tp->peers, cm_entry)
	    assert(p->nwant == 0);
    }
}

/*
 * Called when a piece has been tested negatively.
 */
void
cm_on_bad_piece(struct piece *pc)
{
    struct torrent *tp = pc->tp;

    btpd_log(BTPD_L_ERROR, "Bad hash for piece %u of %s.\n",
        pc->index, tp->relpath);

    for (uint32_t i = 0; i < pc->nblocks; i++) {
	clear_bit(pc->down_field, i);
	clear_bit(pc->have_field, i);
    }
    pc->ngot = 0;
    pc->nbusy = 0;
    msync(tp->imem, tp->isiz, MS_ASYNC);

    if (tp->endgame) {
	struct peer *p;
	BTPDQ_FOREACH(p, &tp->peers, cm_entry) {
	    if (peer_has(p, pc->index) && peer_leech_ok(p) && !peer_laden(p))
		cm_assign_requests_eg(p);
	}
    } else
	cm_on_piece_unfull(pc); // XXX: May get bad data again.
}

void
cm_on_new_peer(struct peer *p)
{
    struct torrent *tp = p->tp;

    tp->npeers++;
    p->flags |= PF_ATTACHED;
    BTPDQ_REMOVE(&btpd.unattached, p, cm_entry);

    if (tp->npeers == 1) {
	BTPDQ_INSERT_HEAD(&tp->peers, p, cm_entry);
	next_optimistic(tp, p);
    } else {
	if (random() > RAND_MAX / 3)
	    BTPDQ_INSERT_AFTER(&tp->peers, tp->optimistic, p, cm_entry);
	else
	    BTPDQ_INSERT_TAIL(&tp->peers, p, cm_entry);
    }
}

void
cm_on_lost_peer(struct peer *p)
{
    struct torrent *tp = p->tp;

    tp->npeers--;
    p->flags &= ~PF_ATTACHED;
    if (tp->npeers == 0) {
	BTPDQ_REMOVE(&tp->peers, p, cm_entry);
	tp->optimistic = NULL;
	tp->choke_time = tp->opt_time = 0;
    } else if (tp->optimistic == p) {
	struct peer *next = BTPDQ_NEXT(p, cm_entry);
	BTPDQ_REMOVE(&tp->peers, p, cm_entry);
	next_optimistic(tp, next);
    } else if ((p->flags & (PF_P_WANT|PF_I_CHOKE)) == PF_P_WANT) {
	BTPDQ_REMOVE(&tp->peers, p, cm_entry);
	cm_on_unupload(p);
    } else {
	BTPDQ_REMOVE(&tp->peers, p, cm_entry);
    }

    for (uint32_t i = 0; i < tp->meta.npieces; i++)
	if (peer_has(p, i))
	    tp->piece_count[i]--;

    if (p->nreqs_out > 0)
	cm_on_undownload(p);
#if 0
    struct piece *pc = BTPDQ_FIRST(&tp->getlst);
    while (pc != NULL) {
	struct piece *next = BTPDQ_NEXT(pc, entry);
	if (peer_has(p, pc->index) && tp->piece_count[pc->index] == 0)
	    cm_on_peerless_piece(pc);
	pc = next;
    }
#endif
}

void
cm_on_block(struct peer *p, struct block_request *req,
    uint32_t index, uint32_t begin, uint32_t length, const char *data)
{
    struct torrent *tp = p->tp;
    struct block *blk = req->blk;
    struct piece *pc = blk->pc;

    BTPDQ_REMOVE(&blk->reqs, req, blk_entry);
    free(req);
    pc->nreqs--;

    off_t cbegin = index * p->tp->meta.piece_length + begin;
    torrent_put_bytes(p->tp, data, cbegin, length);

    set_bit(pc->have_field, begin / PIECE_BLOCKLEN);
    pc->ngot++;

    if (tp->endgame) {
	if (!BTPDQ_EMPTY(&blk->reqs)) {
	    struct net_buf *nb = nb_create_cancel(index, begin, length);
	    nb_hold(nb);
	    struct block_request *req = BTPDQ_FIRST(&blk->reqs);
	    while (req != NULL) {
		struct block_request *next = BTPDQ_NEXT(req, blk_entry);
		peer_cancel(req->p, req, nb);
		free(req);
		pc->nreqs--;
		req = next;
	    }
	    BTPDQ_INIT(&blk->reqs);
	    nb_drop(nb);
	}
	cm_piece_reorder_eg(pc);
	if (pc->ngot == pc->nblocks)
	    cm_on_piece(pc);
	if (peer_leech_ok(p) && !peer_laden(p))
	    cm_assign_requests_eg(p);
    } else {
	// XXX: Needs to be looked at if we introduce snubbing.
	clear_bit(pc->down_field, begin / PIECE_BLOCKLEN);
	pc->nbusy--;
	if (pc->ngot == pc->nblocks)
	    cm_on_piece(pc);
	if (peer_leech_ok(p) && !peer_laden(p))
	    cm_assign_requests(p);
    }
}
