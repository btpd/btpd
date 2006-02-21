/*
 * The commandments:
 *
 * A peer is wanted except when it only has pieces we've already
 * downloaded or fully requested. Thus, a peer's wanted count is
 * increased for each missing or unfull piece it announces, or
 * when a piece it has becomes unfull.
 *
 * When a peer we want unchokes us, requests will primarily
 * be put on pieces we're already downloading and then on
 * possible new pieces.
 *
 * When choosing between several different new pieces to start
 * downloading, the rarest piece will be chosen.
 *
 * End game mode sets in when all missing blocks are requested.
 * In end game mode no piece is counted as full unless it's
 * downloaded.
 *
 */

#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "btpd.h"
#include "stream.h"

static struct piece *
piece_alloc(struct net *n, uint32_t index)
{
    assert(!has_bit(n->busy_field, index)
        && n->npcs_busy < n->tp->meta.npieces);
    struct piece *pc;
    size_t mem, field;
    unsigned nblocks;

    nblocks = torrent_piece_blocks(n->tp, index);
    field = (size_t)ceil(nblocks / 8.0);
    mem = sizeof(*pc) + field;

    pc = btpd_calloc(1, mem);
    pc->n = n;
    pc->down_field = (uint8_t *)(pc + 1);
    pc->have_field = cm_get_block_field(n->tp, index);

    pc->index = index;
    pc->nblocks = nblocks;

    pc->nreqs = 0;
    pc->next_block = 0;

    for (unsigned i = 0; i < nblocks; i++)
        if (has_bit(pc->have_field, i))
            pc->ngot++;
    assert(pc->ngot < pc->nblocks);

    BTPDQ_INIT(&pc->reqs);

    n->npcs_busy++;
    set_bit(n->busy_field, index);
    BTPDQ_INSERT_HEAD(&n->getlst, pc, entry);
    return pc;
}

void
piece_free(struct piece *pc)
{
    struct net *n = pc->n;
    struct block_request *req, *next;
    assert(n->npcs_busy > 0);
    n->npcs_busy--;
    clear_bit(n->busy_field, pc->index);
    BTPDQ_REMOVE(&pc->n->getlst, pc, entry);
    BTPDQ_FOREACH_MUTABLE(req, &pc->reqs, blk_entry, next) {
        nb_drop(req->msg);
        free(req);
    }
    if (pc->eg_reqs != NULL) {
        for (uint32_t i = 0; i < pc->nblocks; i++)
            if (pc->eg_reqs[i] != NULL)
                nb_drop(pc->eg_reqs[i]);
        free(pc->eg_reqs);
    }
    free(pc);
}

int
piece_full(struct piece *pc)
{
    return pc->ngot + pc->nbusy == pc->nblocks;
}

static int
dl_should_enter_endgame(struct net *n)
{
    int should;
    if (cm_pieces(n->tp) + n->npcs_busy == n->tp->meta.npieces) {
        should = 1;
        struct piece *pc;
        BTPDQ_FOREACH(pc, &n->getlst, entry) {
            if (!piece_full(pc)) {
                should = 0;
                break;
            }
        }
    } else
        should = 0;
    return should;
}

static void
dl_piece_insert_eg(struct piece *pc)
{
    struct piece_tq *getlst = &pc->n->getlst;
    if (pc->nblocks == pc->ngot)
        BTPDQ_INSERT_TAIL(getlst, pc, entry);
    else {
        unsigned r = pc->nreqs / (pc->nblocks - pc->ngot);
        struct piece *it;
        BTPDQ_FOREACH(it, getlst, entry) {
            if ((it->nblocks == it->ngot
                    || r < it->nreqs / (it->nblocks - it->ngot))) {
                BTPDQ_INSERT_BEFORE(it, pc, entry);
                break;
            }
        }
        if (it == NULL)
            BTPDQ_INSERT_TAIL(getlst, pc, entry);
    }
}

void
dl_piece_reorder_eg(struct piece *pc)
{
    BTPDQ_REMOVE(&pc->n->getlst, pc, entry);
    dl_piece_insert_eg(pc);
}

static void
dl_enter_endgame(struct net *n)
{
    struct peer *p;
    struct piece *pc;
    struct piece *pcs[n->npcs_busy];
    unsigned pi;

    btpd_log(BTPD_L_POL, "Entering end game\n");
    n->endgame = 1;

    pi = 0;
    BTPDQ_FOREACH(pc, &n->getlst, entry) {
        struct block_request *req;
        for (unsigned i = 0; i < pc->nblocks; i++)
            clear_bit(pc->down_field, i);
        pc->nbusy = 0;
        pc->eg_reqs = btpd_calloc(pc->nblocks, sizeof(struct net_buf *));
        BTPDQ_FOREACH(req, &pc->reqs, blk_entry) {
            uint32_t blki = nb_get_begin(req->msg) / PIECE_BLOCKLEN;
            if (pc->eg_reqs[blki] == NULL) {
                pc->eg_reqs[blki] = req->msg;
                nb_hold(req->msg);
            }
        }
        pcs[pi] = pc;
        pi++;
    }
    BTPDQ_INIT(&n->getlst);
    while (pi > 0) {
        pi--;
        dl_piece_insert_eg(pcs[pi]);
    }
    BTPDQ_FOREACH(p, &n->peers, p_entry) {
        assert(p->nwant == 0);
        BTPDQ_FOREACH(pc, &n->getlst, entry) {
            if (peer_has(p, pc->index))
                peer_want(p, pc->index);
        }
        if (p->nwant > 0 && peer_leech_ok(p) && !peer_laden(p))
            dl_assign_requests_eg(p);
    }
}

struct piece *
dl_find_piece(struct net *n, uint32_t index)
{
    struct piece *pc;
    BTPDQ_FOREACH(pc, &n->getlst, entry)
        if (pc->index == index)
            break;
    return pc;
}

static int
dl_piece_startable(struct peer *p, uint32_t index)
{
    return peer_has(p, index) && !cm_has_piece(p->n->tp, index)
        && !has_bit(p->n->busy_field, index);
}

/*
 * Find the rarest piece the peer has, that isn't already allocated
 * for download or already downloaded. If no such piece can be found
 * return ENOENT.
 *
 * Return 0 or ENOENT, index in res.
 */
static int
dl_choose_rarest(struct peer *p, uint32_t *res)
{
    uint32_t i;
    struct net *n = p->n;

    assert(n->endgame == 0);

    for (i = 0; i < n->tp->meta.npieces && !dl_piece_startable(p, i); i++)
        ;

    if (i == n->tp->meta.npieces)
        return ENOENT;

    uint32_t min_i = i;
    uint32_t min_c = 1;
    for(i++; i < n->tp->meta.npieces; i++) {
        if (dl_piece_startable(p, i)) {
            if (n->piece_count[i] == n->piece_count[min_i])
                min_c++;
            else if (n->piece_count[i] < n->piece_count[min_i]) {
                min_i = i;
                min_c = 1;
            }
        }
    }
    if (min_c > 1) {
        min_c = rand_between(1, min_c);
        for (i = min_i; min_c > 0; i++) {
            if (dl_piece_startable(p, i)
                && n->piece_count[i] == n->piece_count[min_i]) {
                min_c--;
                min_i = i;
            }
        }
    }
    *res = min_i;
    return 0;
}

/*
 * Called from dl_piece_assign_requests when a piece becomes full.
 * The wanted level of the peers that has this piece will be decreased.
 * This function is the only one that may trigger end game.
 */
static void
dl_on_piece_full(struct piece *pc)
{
    struct peer *p;
    BTPDQ_FOREACH(p, &pc->n->peers, p_entry) {
        if (peer_has(p, pc->index))
            peer_unwant(p, pc->index);
    }
    if (dl_should_enter_endgame(pc->n))
        dl_enter_endgame(pc->n);
}

/*
 * Allocate the piece indicated by the index for download.
 * There's a small possibility that a piece is fully downloaded
 * but haven't been tested. If such is the case the piece will
 * be tested and NULL will be returned. Also, we might then enter
 * end game.
 *
 * Return the piece or NULL.
 */
struct piece *
dl_new_piece(struct net *n, uint32_t index)
{
    btpd_log(BTPD_L_POL, "Started on piece %u.\n", index);
    cm_prealloc(n->tp, index);
    return piece_alloc(n, index);
}

/*
 * Called when a previously full piece loses a peer.
 * This is needed because we have decreased the wanted
 * level for the peers that have this piece when it got
 * full. Thus we have to increase the wanted level and
 * try to assign requests for this piece.
 */
void
dl_on_piece_unfull(struct piece *pc)
{
    struct net *n = pc->n;
    struct peer *p;
    assert(!piece_full(pc) && n->endgame == 0);
    BTPDQ_FOREACH(p, &n->peers, p_entry)
        if (peer_has(p, pc->index))
            peer_want(p, pc->index);
    p = BTPDQ_FIRST(&n->peers);
    while (p != NULL && !piece_full(pc)) {
        if (peer_leech_ok(p) && !peer_laden(p))
            dl_piece_assign_requests(pc, p); // Cannot provoke end game here.
        p = BTPDQ_NEXT(p, p_entry);
    }
}

#define INCNEXTBLOCK(pc) \
    (pc)->next_block = ((pc)->next_block + 1) % (pc)->nblocks

static struct block_request *
dl_new_request(struct peer *p, struct piece *pc, struct net_buf *msg)
{
    if (msg == NULL) {
        uint32_t block = pc->next_block;
        uint32_t start = block * PIECE_BLOCKLEN;
        uint32_t length =
            torrent_block_size(pc->n->tp, pc->index, pc->nblocks, block);
        msg = nb_create_request(pc->index, start, length);
    }
    struct block_request *req = btpd_malloc(sizeof(*req));
    req->p = p;
    req->msg = msg;
    nb_hold(req->msg);
    BTPDQ_INSERT_TAIL(&pc->reqs, req, blk_entry);
    pc->nreqs++;
    if (!pc->n->endgame) {
        set_bit(pc->down_field, pc->next_block);
        pc->nbusy++;
    }
    INCNEXTBLOCK(pc);
    peer_request(p, req);
    return req;
}

/*
 * Request as many blocks as possible on this piece from
 * the peer. If the piece becomes full we call dl_on_piece_full.
 *
 * Return the number of requests sent.
 */
unsigned
dl_piece_assign_requests(struct piece *pc, struct peer *p)
{
    assert(!piece_full(pc) && !peer_laden(p));
    unsigned count = 0;
    do {
        while ((has_bit(pc->have_field, pc->next_block)
                   || has_bit(pc->down_field, pc->next_block)))
            INCNEXTBLOCK(pc);
        dl_new_request(p, pc, NULL);
        count++;
    } while (!piece_full(pc) && !peer_laden(p));

    if (piece_full(pc))
        dl_on_piece_full(pc);

    return count;
}

/*
 * Request as many blocks as possible from the peer. Puts
 * requests on already active pieces before starting on new
 * ones. Care must be taken since end game mode may be triggered
 * by the calls to dl_piece_assign_requests.
 *
 * Returns number of requests sent.
 *
 * XXX: should do something smart when deciding on which
 *      already started piece to put requests on.
 */
unsigned
dl_assign_requests(struct peer *p)
{
    assert(!p->n->endgame && !peer_laden(p));
    struct piece *pc;
    struct net *n = p->n;
    unsigned count = 0;
    BTPDQ_FOREACH(pc, &n->getlst, entry) {
        if (piece_full(pc) || !peer_has(p, pc->index))
            continue;
        count += dl_piece_assign_requests(pc, p);
        if (n->endgame)
            break;
        if (!piece_full(pc))
            assert(peer_laden(p));
        if (peer_laden(p))
            break;
    }
    while (!peer_laden(p) && !n->endgame) {
        uint32_t index;
        if (dl_choose_rarest(p, &index) == 0) {
            pc = dl_new_piece(n, index);
            if (pc != NULL)
                count += dl_piece_assign_requests(pc, p);
        } else
            break;
    }
    return count;
}

void
dl_unassign_requests(struct peer *p)
{
    while (p->nreqs_out > 0) {
        struct block_request *req = BTPDQ_FIRST(&p->my_reqs);
        struct piece *pc = dl_find_piece(p->n, nb_get_index(req->msg));
        int was_full = piece_full(pc);

        while (req != NULL) {
            struct block_request *next = BTPDQ_NEXT(req, p_entry);

            uint32_t blki = nb_get_begin(req->msg) / PIECE_BLOCKLEN;
            // XXX: Needs to be looked at if we introduce snubbing.
            assert(has_bit(pc->down_field, blki));
            clear_bit(pc->down_field, blki);
            pc->nbusy--;
            BTPDQ_REMOVE(&p->my_reqs, req, p_entry);
            p->nreqs_out--;
            BTPDQ_REMOVE(&pc->reqs, req, blk_entry);
            nb_drop(req->msg);
            free(req);
            pc->nreqs--;

            while (next != NULL && nb_get_index(next->msg) != pc->index)
                next = BTPDQ_NEXT(next, p_entry);
            req = next;
        }

        if (p->nreqs_out == 0)
            peer_on_no_reqs(p);

        if (was_full && !piece_full(pc))
            dl_on_piece_unfull(pc);
    }
    assert(BTPDQ_EMPTY(&p->my_reqs));
}

static void
dl_piece_assign_requests_eg(struct piece *pc, struct peer *p)
{
    unsigned first_block = pc->next_block;
    do {
        if ((has_bit(pc->have_field, pc->next_block)
                || peer_requested(p, pc->index, pc->next_block))) {
            INCNEXTBLOCK(pc);
            continue;
        }
        struct block_request *req = 
            dl_new_request(p, pc, pc->eg_reqs[pc->next_block]);
        if (pc->eg_reqs[pc->next_block] == NULL) {
            pc->eg_reqs[pc->next_block] = req->msg;
            nb_hold(req->msg);
        }
    } while (!peer_laden(p) && pc->next_block != first_block);
}

void
dl_assign_requests_eg(struct peer *p)
{
    assert(!peer_laden(p));
    struct net *n = p->n;
    struct piece_tq tmp;
    BTPDQ_INIT(&tmp);

    struct piece *pc = BTPDQ_FIRST(&n->getlst);
    while (!peer_laden(p) && pc != NULL) {
        struct piece *next = BTPDQ_NEXT(pc, entry);
        if (peer_has(p, pc->index) && pc->nblocks != pc->ngot) {
            dl_piece_assign_requests_eg(pc, p);
            BTPDQ_REMOVE(&n->getlst, pc, entry);
            BTPDQ_INSERT_HEAD(&tmp, pc, entry);
        }
        pc = next;
    }

    pc = BTPDQ_FIRST(&tmp);
    while (pc != NULL) {
        struct piece *next = BTPDQ_NEXT(pc, entry);
        dl_piece_insert_eg(pc);
        pc = next;
    }
}

void
dl_unassign_requests_eg(struct peer *p)
{
    struct block_request *req;
    struct piece *pc;
    struct piece_tq tmp;
    BTPDQ_INIT(&tmp);

    while (p->nreqs_out > 0) {
        req = BTPDQ_FIRST(&p->my_reqs);

        pc = dl_find_piece(p->n, nb_get_index(req->msg));
        BTPDQ_REMOVE(&pc->n->getlst, pc, entry);
        BTPDQ_INSERT_HEAD(&tmp, pc, entry);

        while (req != NULL) {
            struct block_request *next = BTPDQ_NEXT(req, p_entry);
            BTPDQ_REMOVE(&p->my_reqs, req, p_entry);
            p->nreqs_out--;
            BTPDQ_REMOVE(&pc->reqs, req, blk_entry);
            nb_drop(req->msg);
            free(req);
            pc->nreqs--;

            while (next != NULL && nb_get_index(next->msg) != pc->index)
                next = BTPDQ_NEXT(next, p_entry);
            req = next;
        }
    }
    assert(BTPDQ_EMPTY(&p->my_reqs));
    peer_on_no_reqs(p);

    pc = BTPDQ_FIRST(&tmp);
    while (pc != NULL) {
        struct piece *next = BTPDQ_NEXT(pc, entry);
        dl_piece_insert_eg(pc);
        pc = next;
    }
}
