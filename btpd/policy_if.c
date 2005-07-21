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
    struct piece *pc = torrent_get_piece(tp, index);
    if (tp->endgame) {
	if (pc != NULL) {
	    peer_want(p, index);
	    if (!peer_chokes(p))
		cm_piece_assign_requests_eg(pc, p);
	}
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
    } else if (cm_assign_requests(p) == 0)
	assert(!peer_wanted(p) || peer_laden(p));
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

    BTPDQ_FOREACH(p, &tp->peers, cm_entry)
	peer_have(p, pc->index);

    if (tp->endgame)
	BTPDQ_FOREACH(p, &tp->peers, cm_entry)
	    if (peer_has(p, pc->index))
		peer_unwant(p, pc->index);

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
	    if (peer_has(p, pc->index) && peer_leech_ok(p))
		cm_piece_assign_requests_eg(pc, p);
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
cm_on_block(struct peer *p, uint32_t index, uint32_t begin, uint32_t length,
    const char *data)
{
    struct torrent *tp = p->tp;

    off_t cbegin = index * p->tp->meta.piece_length + begin;
    torrent_put_bytes(p->tp, data, cbegin, length);

    struct piece *pc = BTPDQ_FIRST(&tp->getlst);
    while (pc != NULL && pc->index != index)
        pc = BTPDQ_NEXT(pc, entry);
    uint32_t block = begin / PIECE_BLOCKLEN;
    set_bit(pc->have_field, block);
    clear_bit(pc->down_field, block);
    pc->ngot++;
    pc->nbusy--;

    if (tp->endgame) {
	BTPDQ_FOREACH(p, &tp->peers, cm_entry) {
	    if (peer_has(p, index) && peer_leech_ok(p))
		peer_cancel(p, index, begin, length);
	}
	if (pc->ngot == pc->nblocks)
	    cm_on_piece(pc);
    } else {
	if (pc->ngot == pc->nblocks)
	    cm_on_piece(pc);
	if (peer_leech_ok(p))
	    cm_assign_requests(p);
    }
}
