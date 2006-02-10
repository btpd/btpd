#include <math.h>

#include "btpd.h"
#include "tracker_req.h"

/*
 * Called when a peer announces it's got a new piece.
 *
 * If the piece is missing or unfull we increase the peer's
 * wanted level and if possible call dl_on_download.
 */
void
dl_on_piece_ann(struct peer *p, uint32_t index)
{
    struct net *n = p->n;
    n->piece_count[index]++;
    if (cm_has_piece(n->tp, index))
        return;
    struct piece *pc = dl_find_piece(n, index);
    if (n->endgame) {
        assert(pc != NULL);
        peer_want(p, index);
        if (!peer_chokes(p) && !peer_laden(p))
            dl_assign_requests_eg(p);
    } else if (pc == NULL) {
        peer_want(p, index);
        if (!peer_chokes(p) && !peer_laden(p)) {
            pc = dl_new_piece(n, index);
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
    struct net *n = p->n;
    if (n->endgame) {
        dl_assign_requests_eg(p);
    } else {
        unsigned count = dl_assign_requests(p);
        if (count == 0 && !p->n->endgame) // We may have entered end game.
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
    if (!p->n->endgame)
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
dl_on_ok_piece(struct net *n, uint32_t piece)
{
    struct peer *p;
    struct piece *pc = dl_find_piece(n, piece);

    btpd_log(BTPD_L_POL, "Got piece: %u.\n", pc->index);

    struct net_buf *have = nb_create_have(pc->index);
    BTPDQ_FOREACH(p, &n->peers, p_entry)
        peer_send(p, have);

    if (n->endgame)
        BTPDQ_FOREACH(p, &n->peers, p_entry)
            if (peer_has(p, pc->index))
                peer_unwant(p, pc->index);

    assert(pc->nreqs == 0);
    piece_free(pc);

    if (cm_full(n->tp)) {
        btpd_log(BTPD_L_BTPD, "Finished downloading '%s'.\n",
            torrent_name(n->tp));
        tr_complete(n->tp);
        BTPDQ_FOREACH(p, &n->peers, p_entry)
            assert(p->nwant == 0);
    }
}

/*
 * Called when a piece has been tested negatively.
 */
void
dl_on_bad_piece(struct net *n, uint32_t piece)
{
    struct piece *pc = dl_find_piece(n, piece);

    btpd_log(BTPD_L_ERROR, "Bad hash for piece %u of '%s'.\n",
        pc->index, torrent_name(n->tp));

    for (uint32_t i = 0; i < pc->nblocks; i++)
        clear_bit(pc->down_field, i);

    pc->ngot = 0;
    pc->nbusy = 0;

    if (n->endgame) {
        struct peer *p;
        BTPDQ_FOREACH(p, &n->peers, p_entry) {
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
    struct net *n = p->n;

    for (uint32_t i = 0; i < n->tp->meta.npieces; i++)
        if (peer_has(p, i))
            n->piece_count[i]--;

    if (p->nreqs_out > 0)
        dl_on_undownload(p);
}

void
dl_on_block(struct peer *p, struct block_request *req,
    uint32_t index, uint32_t begin, uint32_t length, const uint8_t *data)
{
    struct net *n = p->n;
    struct block *blk = req->blk;
    struct piece *pc = blk->pc;

    cm_put_bytes(p->n->tp, index, begin, data, length);
    pc->ngot++;

    if (n->endgame) {
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
            cm_test_piece(pc->n->tp, pc->index);
    } else {
        BTPDQ_REMOVE(&blk->reqs, req, blk_entry);
        free(req);
        pc->nreqs--;
        // XXX: Needs to be looked at if we introduce snubbing.
        clear_bit(pc->down_field, begin / PIECE_BLOCKLEN);
        pc->nbusy--;
        if (pc->ngot == pc->nblocks)
            cm_test_piece(pc->n->tp, pc->index);
        if (peer_leech_ok(p) && !peer_laden(p))
            dl_assign_requests(p);
    }
}
