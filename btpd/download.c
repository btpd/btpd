#include <math.h>

#include "btpd.h"
#include "tracker_req.h"

void
dl_start(struct torrent *tp)
{
    BTPDQ_INIT(&tp->getlst);
    tp->busy_field = btpd_calloc((size_t)ceil(tp->meta.npieces / 8.0), 1);
    tp->piece_count = btpd_calloc(tp->meta.npieces,
        sizeof(*(tp->piece_count)));
}

void
dl_stop(struct torrent *tp)
{
    struct piece *pc;
    while ((pc = BTPDQ_FIRST(&tp->getlst)) != NULL)
        piece_free(pc);
    free(tp->busy_field);
    free(tp->piece_count);
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
    if (cm_has_piece(tp, index))
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

/**
 * Called when a piece has been tested positively.
 */
void
dl_on_ok_piece(struct piece *pc)
{
    struct peer *p;
    struct torrent *tp = pc->tp;

    btpd_log(BTPD_L_POL, "Got piece: %u.\n", pc->index);

    struct net_buf *have = nb_create_have(pc->index);
    BTPDQ_FOREACH(p, &tp->peers, p_entry)
        peer_send(p, have);

    if (tp->endgame)
        BTPDQ_FOREACH(p, &tp->peers, p_entry)
            if (peer_has(p, pc->index))
                peer_unwant(p, pc->index);

    assert(pc->nreqs == 0);
    piece_free(pc);

    if (cm_full(tp)) {
        btpd_log(BTPD_L_BTPD, "Finished: %s.\n", tp->relpath);
        tr_complete(tp);
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

    for (uint32_t i = 0; i < pc->nblocks; i++)
        clear_bit(pc->down_field, i);

    pc->ngot = 0;
    pc->nbusy = 0;

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
}

void
dl_on_lost_peer(struct peer *p)
{
    struct torrent *tp = p->tp;

    for (uint32_t i = 0; i < tp->meta.npieces; i++)
        if (peer_has(p, i))
            tp->piece_count[i]--;

    if (p->nreqs_out > 0)
        dl_on_undownload(p);
}

void
dl_on_block(struct peer *p, struct block_request *req,
    uint32_t index, uint32_t begin, uint32_t length, const char *data)
{
    struct torrent *tp = p->tp;
    struct block *blk = req->blk;
    struct piece *pc = blk->pc;

    cm_put_block(p->tp, index, begin / PIECE_BLOCKLEN, data);
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
            cm_test_piece(pc);
    } else {
        BTPDQ_REMOVE(&blk->reqs, req, blk_entry);
        free(req);
        pc->nreqs--;
        // XXX: Needs to be looked at if we introduce snubbing.
        clear_bit(pc->down_field, begin / PIECE_BLOCKLEN);
        pc->nbusy--;
        if (pc->ngot == pc->nblocks)
            cm_test_piece(pc);
        if (peer_leech_ok(p) && !peer_laden(p))
            dl_assign_requests(p);
    }
}
