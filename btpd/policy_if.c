#include <sys/types.h>
#include <sys/mman.h>

#include "btpd.h"
#include "tracker_req.h"

void
dl_by_second(struct torrent *tp)
{
    if (btpd_seconds == tp->tracker_time)
	tracker_req(tp, TR_EMPTY);

    if (btpd_seconds == tp->opt_time)
	next_optimistic(tp, NULL);

    if (btpd_seconds == tp->choke_time)
	choke_alg(tp);

    struct peer *p;
    int ri = btpd_seconds % RATEHISTORY;

    BTPDQ_FOREACH(p, &tp->peers, p_entry) {
	p->rate_to_me[ri] = 0;
	p->rate_from_me[ri] = 0;
    }
}

/*
 * Called when a peer announces it's got a new piece.
 *
 * If the piece is missing or unfull we increase the peer's
 * wanted level and if possible call dl_on_download.
 */
void
dl_on_piece_ann(struct peer *p, uint32_t index)
{
    struct torrent *tp = p->tp;
    tp->piece_count[index]++;
    if (has_bit(tp->piece_field, index))
	return;
    struct piece *pc = dl_find_piece(tp, index);
    if (tp->endgame) {
	assert(pc != NULL);
	peer_want(p, index);
	if (!peer_chokes(p) && !peer_laden(p))
	    dl_assign_requests_eg(p);
    } else if (pc == NULL) {
	peer_want(p, index);
	if (!peer_chokes(p) && !peer_laden(p)) {
	    pc = dl_new_piece(tp, index);
	    if (pc != NULL)
		dl_piece_assign_requests(pc, p);
	}
    } else if (!piece_full(pc)) {
	peer_want(p, index);
	if (!peer_chokes(p) && !peer_laden(p))
	    dl_piece_assign_requests(pc, p);
    }
}

void
dl_on_download(struct peer *p)
{
    assert(peer_wanted(p));
    struct torrent *tp = p->tp;
    if (tp->endgame) {
	dl_assign_requests_eg(p);
    } else {
	unsigned count = dl_assign_requests(p);
	if (count == 0 && !p->tp->endgame) // We may have entered end game.
	    assert(!peer_wanted(p) || peer_laden(p));
    }
}

void
dl_on_unchoke(struct peer *p)
{
    if (peer_wanted(p))
	dl_on_download(p);
}

void
dl_on_undownload(struct peer *p)
{
    if (!p->tp->endgame)
	dl_unassign_requests(p);
    else
	dl_unassign_requests_eg(p);
}

void
dl_on_choke(struct peer *p)
{
    if (p->nreqs_out > 0)
	dl_on_undownload(p);
}

void
dl_on_upload(struct peer *p)
{
    choke_alg(p->tp);
}

void
dl_on_interest(struct peer *p)
{
    if ((p->flags & PF_I_CHOKE) == 0)
	dl_on_upload(p);
}

void
dl_on_unupload(struct peer *p)
{
    choke_alg(p->tp);
}

void
dl_on_uninterest(struct peer *p)
{
    if ((p->flags & PF_I_CHOKE) == 0)
	dl_on_unupload(p);
}

/**
 * Called when a piece has been tested positively.
 */
void
dl_on_ok_piece(struct piece *pc)
{
    struct peer *p;
    struct torrent *tp = pc->tp;

    btpd_log(BTPD_L_POL, "Got piece: %u.\n", pc->index);

    set_bit(tp->piece_field, pc->index);
    tp->have_npieces++;
    msync(tp->imem, tp->isiz, MS_ASYNC);

    struct net_buf *have = nb_create_have(pc->index);
    BTPDQ_FOREACH(p, &tp->peers, p_entry)
	peer_send(p, have);

    if (tp->endgame)
	BTPDQ_FOREACH(p, &tp->peers, p_entry)
	    if (peer_has(p, pc->index))
		peer_unwant(p, pc->index);

    assert(pc->nreqs == 0);
    piece_free(pc);

    if (torrent_has_all(tp)) {
	btpd_log(BTPD_L_BTPD, "Finished: %s.\n", tp->relpath);
	tracker_req(tp, TR_COMPLETED);
	BTPDQ_FOREACH(p, &tp->peers, p_entry)
	    assert(p->nwant == 0);
    }
}

/*
 * Called when a piece has been tested negatively.
 */
void
dl_on_bad_piece(struct piece *pc)
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
	BTPDQ_FOREACH(p, &tp->peers, p_entry) {
	    if (peer_has(p, pc->index) && peer_leech_ok(p) && !peer_laden(p))
		dl_assign_requests_eg(p);
	}
    } else
	dl_on_piece_unfull(pc); // XXX: May get bad data again.
}

void
dl_on_new_peer(struct peer *p)
{
    struct torrent *tp = p->tp;

    tp->npeers++;
    p->flags |= PF_ATTACHED;
    BTPDQ_REMOVE(&net_unattached, p, p_entry);

    if (tp->npeers == 1) {
	BTPDQ_INSERT_HEAD(&tp->peers, p, p_entry);
	next_optimistic(tp, p);
    } else {
	if (random() > RAND_MAX / 3)
	    BTPDQ_INSERT_AFTER(&tp->peers, tp->optimistic, p, p_entry);
	else
	    BTPDQ_INSERT_TAIL(&tp->peers, p, p_entry);
    }
}

void
dl_on_lost_peer(struct peer *p)
{
    struct torrent *tp = p->tp;

    tp->npeers--;
    p->flags &= ~PF_ATTACHED;
    if (tp->npeers == 0) {
	BTPDQ_REMOVE(&tp->peers, p, p_entry);
	tp->optimistic = NULL;
	tp->choke_time = tp->opt_time = 0;
    } else if (tp->optimistic == p) {
	struct peer *next = BTPDQ_NEXT(p, p_entry);
	BTPDQ_REMOVE(&tp->peers, p, p_entry);
	next_optimistic(tp, next);
    } else if ((p->flags & (PF_P_WANT|PF_I_CHOKE)) == PF_P_WANT) {
	BTPDQ_REMOVE(&tp->peers, p, p_entry);
	dl_on_unupload(p);
    } else {
	BTPDQ_REMOVE(&tp->peers, p, p_entry);
    }

    for (uint32_t i = 0; i < tp->meta.npieces; i++)
	if (peer_has(p, i))
	    tp->piece_count[i]--;

    if (p->nreqs_out > 0)
	dl_on_undownload(p);
#if 0
    struct piece *pc = BTPDQ_FIRST(&tp->getlst);
    while (pc != NULL) {
	struct piece *next = BTPDQ_NEXT(pc, entry);
	if (peer_has(p, pc->index) && tp->piece_count[pc->index] == 0)
	    dl_on_peerless_piece(pc);
	pc = next;
    }
#endif
}

void
dl_on_block(struct peer *p, struct block_request *req,
    uint32_t index, uint32_t begin, uint32_t length, const char *data)
{
    struct torrent *tp = p->tp;
    struct block *blk = req->blk;
    struct piece *pc = blk->pc;

    off_t cbegin = index * p->tp->meta.piece_length + begin;
    torrent_put_bytes(p->tp, data, cbegin, length);

    set_bit(pc->have_field, begin / PIECE_BLOCKLEN);
    pc->ngot++;

    if (tp->endgame) {
        struct block_request *req;
        struct net_buf *cancel = nb_create_cancel(index, begin, length);
        nb_hold(cancel);
        BTPDQ_FOREACH(req, &blk->reqs, blk_entry) {
            if (req->p != p)
                peer_cancel(req->p, req, cancel);
            pc->nreqs--;
        }
        nb_drop(cancel);
        dl_piece_reorder_eg(pc);
        req = BTPDQ_FIRST(&blk->reqs);
        while (req != NULL) {
            struct block_request *next = BTPDQ_NEXT(req, blk_entry);
            if (peer_leech_ok(req->p) && !peer_laden(req->p))
                dl_assign_requests_eg(req->p);
            free(req);
            req = next;
        }
        BTPDQ_INIT(&blk->reqs);
        if (pc->ngot == pc->nblocks)
            dl_on_piece(pc);
    } else {
        BTPDQ_REMOVE(&blk->reqs, req, blk_entry);
        free(req);
        pc->nreqs--;
	// XXX: Needs to be looked at if we introduce snubbing.
	clear_bit(pc->down_field, begin / PIECE_BLOCKLEN);
	pc->nbusy--;
	if (pc->ngot == pc->nblocks)
	    dl_on_piece(pc);
	if (peer_leech_ok(p) && !peer_laden(p))
	    dl_assign_requests(p);
    }
}
