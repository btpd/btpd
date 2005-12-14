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
piece_alloc(struct torrent *tp, uint32_t index)
{
    assert(!has_bit(tp->busy_field, index)
        && tp->npcs_busy < tp->meta.npieces);
    struct piece *pc;
    size_t mem, field, blocks;
    unsigned nblocks;
    off_t piece_length = torrent_piece_size(tp, index);

    nblocks = (unsigned)ceil((double)piece_length / PIECE_BLOCKLEN);
    blocks = sizeof(pc->blocks[0]) * nblocks;
    field = (size_t)ceil(nblocks / 8.0);
    mem = sizeof(*pc) + field + blocks;

    pc = btpd_calloc(1, mem);
    pc->tp = tp;
    pc->down_field = (uint8_t *)(pc + 1);
    pc->have_field =
        tp->block_field +
        index * (size_t)ceil(tp->meta.piece_length / (double)(1 << 17));

    pc->index = index;
    pc->nblocks = nblocks;

    pc->nreqs = 0;
    pc->next_block = 0;

    for (unsigned i = 0; i < nblocks; i++)
        if (has_bit(pc->have_field, i))
            pc->ngot++;

    pc->blocks = (struct block *)(pc->down_field + field);
    for (unsigned i = 0; i < nblocks; i++) {
        uint32_t start = i * PIECE_BLOCKLEN;
        uint32_t len = torrent_block_size(pc, i);
        struct block *blk = &pc->blocks[i];
        blk->pc = pc;
        BTPDQ_INIT(&blk->reqs);
        blk->msg = nb_create_request(index, start, len);
        nb_hold(blk->msg);
    }

    tp->npcs_busy++;
    set_bit(tp->busy_field, index);
    BTPDQ_INSERT_HEAD(&tp->getlst, pc, entry);
    return pc;
}

void
piece_free(struct piece *pc)
{
    struct torrent *tp = pc->tp;
    assert(tp->npcs_busy > 0);
    tp->npcs_busy--;
    clear_bit(tp->busy_field, pc->index);
    BTPDQ_REMOVE(&pc->tp->getlst, pc, entry);
    for (unsigned i = 0; i < pc->nblocks; i++) {
        struct block_request *req = BTPDQ_FIRST(&pc->blocks[i].reqs);
        while (req != NULL) {
            struct block_request *next = BTPDQ_NEXT(req, blk_entry);
            free(req);
            req = next;
        }
        nb_drop(pc->blocks[i].msg);
    }
    free(pc);
}

int
piece_full(struct piece *pc)
{
    return pc->ngot + pc->nbusy == pc->nblocks;
}

static int
dl_should_enter_endgame(struct torrent *tp)
{
    int should;
    if (tp->have_npieces + tp->npcs_busy == tp->meta.npieces) {
        should = 1;
        struct piece *pc;
        BTPDQ_FOREACH(pc, &tp->getlst, entry) {
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
    struct piece_tq *getlst = &pc->tp->getlst;
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
    BTPDQ_REMOVE(&pc->tp->getlst, pc, entry);
    dl_piece_insert_eg(pc);
}

static void
dl_enter_endgame(struct torrent *tp)
{
    struct peer *p;
    struct piece *pc;
    struct piece *pcs[tp->npcs_busy];
    unsigned pi;

    btpd_log(BTPD_L_POL, "Entering end game\n");
    tp->endgame = 1;

    pi = 0;
    BTPDQ_FOREACH(pc, &tp->getlst, entry) {
        for (unsigned i = 0; i < pc->nblocks; i++)
            clear_bit(pc->down_field, i);
        pc->nbusy = 0;
        pcs[pi] = pc;
        pi++;
    }
    BTPDQ_INIT(&tp->getlst);
    while (pi > 0) {
        pi--;
        dl_piece_insert_eg(pcs[pi]);
    }
    BTPDQ_FOREACH(p, &tp->peers, p_entry) {
        assert(p->nwant == 0);
        BTPDQ_FOREACH(pc, &tp->getlst, entry) {
            if (peer_has(p, pc->index))
                peer_want(p, pc->index);
        }
        if (p->nwant > 0 && peer_leech_ok(p) && !peer_laden(p))
            dl_assign_requests_eg(p);
    }
}

struct piece *
dl_find_piece(struct torrent *tp, uint32_t index)
{
    struct piece *pc;
    BTPDQ_FOREACH(pc, &tp->getlst, entry)
        if (pc->index == index)
            break;
    return pc;
}

static int
test_hash(struct torrent *tp, uint8_t *hash, unsigned long index)
{
    if (tp->meta.piece_hash != NULL)
        return memcmp(hash, tp->meta.piece_hash[index], SHA_DIGEST_LENGTH);
    else {
        char piece_hash[SHA_DIGEST_LENGTH];
        int fd;
        int bufi;
        int err;

        err = vopen(&fd, O_RDONLY, "%s/torrent", tp->relpath);
        if (err != 0)
            btpd_err("test_hash: %s\n", strerror(err));

        err = lseek(fd, tp->meta.pieces_off + index * SHA_DIGEST_LENGTH,
            SEEK_SET);
        if (err < 0)
            btpd_err("test_hash: %s\n", strerror(errno));

        bufi = 0;
        while (bufi < SHA_DIGEST_LENGTH) {
            ssize_t nread =
                read(fd, piece_hash + bufi, SHA_DIGEST_LENGTH - bufi);
            bufi += nread;
        }
        close(fd);

        return memcmp(hash, piece_hash, SHA_DIGEST_LENGTH);
    }
}

static int
ro_fd_cb(const char *path, int *fd, void *arg)
{
    struct torrent *tp = arg;
    return vopen(fd, O_RDONLY, "%s/content/%s", tp->relpath, path);
}

static void
torrent_test_piece(struct piece *pc)
{
    struct torrent *tp = pc->tp;
    int err;
    uint8_t hash[20];
    struct bt_stream_ro *bts;
    off_t plen = torrent_piece_size(tp, pc->index);

    if ((bts = bts_open_ro(&tp->meta, pc->index * tp->meta.piece_length,
             ro_fd_cb, tp)) == NULL)
        btpd_err("Out of memory.\n");

    if ((err = bts_sha(bts, plen, hash)) != 0)
        btpd_err("Ouch! %s\n", strerror(err));

    bts_close_ro(bts);

    if (test_hash(tp, hash, pc->index) == 0)
        dl_on_ok_piece(pc);
    else
        dl_on_bad_piece(pc);
}

void
dl_on_piece(struct piece *pc)
{
    torrent_test_piece(pc);
}

static int
dl_piece_startable(struct peer *p, uint32_t index)
{
    return peer_has(p, index) && !has_bit(p->tp->piece_field, index)
        && !has_bit(p->tp->busy_field, index);
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
    struct torrent *tp = p->tp;

    assert(tp->endgame == 0);

    for (i = 0; i < tp->meta.npieces && !dl_piece_startable(p, i); i++)
        ;

    if (i == tp->meta.npieces)
        return ENOENT;

    uint32_t min_i = i;
    uint32_t min_c = 1;
    for(i++; i < tp->meta.npieces; i++) {
        if (dl_piece_startable(p, i)) {
            if (tp->piece_count[i] == tp->piece_count[min_i])
                min_c++;
            else if (tp->piece_count[i] < tp->piece_count[min_i]) {
                min_i = i;
                min_c = 1;
            }
        }
    }
    if (min_c > 1) {
        min_c = rand_between(1, min_c);
        for (i = min_i; min_c > 0; i++) {
            if (dl_piece_startable(p, i)
                && tp->piece_count[i] == tp->piece_count[min_i]) {
                min_c--;
                min_i = i;
            }
        }
    }
    *res = min_i;
    return 0;
}

/*
 * Called from either dl_piece_assign_requests or dl_new_piece,
 * when a pice becomes full. The wanted level of the peers
 * that has this piece will be decreased. This function is
 * the only one that may trigger end game.
 */
static void
dl_on_piece_full(struct piece *pc)
{
    struct peer *p;
    BTPDQ_FOREACH(p, &pc->tp->peers, p_entry) {
        if (peer_has(p, pc->index))
            peer_unwant(p, pc->index);
    }
    if (dl_should_enter_endgame(pc->tp))
        dl_enter_endgame(pc->tp);
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
dl_new_piece(struct torrent *tp, uint32_t index)
{
    btpd_log(BTPD_L_POL, "Started on piece %u.\n", index);
    struct piece *pc = piece_alloc(tp, index);
    if (pc->ngot == pc->nblocks) {
        dl_on_piece_full(pc);
        dl_on_piece(pc);
        if (dl_should_enter_endgame(tp))
            dl_enter_endgame(tp);
        return NULL;
    } else
        return pc;
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
    struct torrent *tp = pc->tp;
    struct peer *p;
    assert(!piece_full(pc) && tp->endgame == 0);
    BTPDQ_FOREACH(p, &tp->peers, p_entry)
        if (peer_has(p, pc->index))
            peer_want(p, pc->index);
    p = BTPDQ_FIRST(&tp->peers);
    while (p != NULL && !piece_full(pc)) {
        if (peer_leech_ok(p) && !peer_laden(p))
            dl_piece_assign_requests(pc, p); // Cannot provoke end game here.
        p = BTPDQ_NEXT(p, p_entry);
    }
}

#define INCNEXTBLOCK(pc) \
    (pc)->next_block = ((pc)->next_block + 1) % (pc)->nblocks


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

        struct block *blk = &pc->blocks[pc->next_block];
        struct block_request *req = btpd_malloc(sizeof(*req));
        req->p = p;
        req->blk = blk;
        BTPDQ_INSERT_TAIL(&blk->reqs, req, blk_entry);

        peer_request(p, req);

        set_bit(pc->down_field, pc->next_block);
        pc->nbusy++;
        pc->nreqs++;
        count++;
        INCNEXTBLOCK(pc);
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
    assert(!p->tp->endgame && !peer_laden(p));
    struct piece *pc;
    struct torrent *tp = p->tp;
    unsigned count = 0;
    BTPDQ_FOREACH(pc, &tp->getlst, entry) {
        if (piece_full(pc) || !peer_has(p, pc->index))
            continue;
        count += dl_piece_assign_requests(pc, p);
        if (tp->endgame)
            break;
        if (!piece_full(pc))
            assert(peer_laden(p));
        if (peer_laden(p))
            break;
    }
    while (!peer_laden(p) && !tp->endgame) {
        uint32_t index;
        if (dl_choose_rarest(p, &index) == 0) {
            pc = dl_new_piece(tp, index);
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
        struct piece *pc = req->blk->pc;
        int was_full = piece_full(pc);

        while (req != NULL) {
            struct block_request *next = BTPDQ_NEXT(req, p_entry);

            uint32_t blki = nb_get_begin(req->blk->msg) / PIECE_BLOCKLEN;
            struct block *blk = req->blk;
            // XXX: Needs to be looked at if we introduce snubbing.
            assert(has_bit(pc->down_field, blki));
            clear_bit(pc->down_field, blki);
            pc->nbusy--;
            BTPDQ_REMOVE(&p->my_reqs, req, p_entry);
            p->nreqs_out--;
            BTPDQ_REMOVE(&blk->reqs, req, blk_entry);
            free(req);
            pc->nreqs--;

            while (next != NULL && next->blk->pc != pc)
                next = BTPDQ_NEXT(next, p_entry);
            req = next;
        }

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
                || peer_requested(p, &pc->blocks[pc->next_block]))) {
            INCNEXTBLOCK(pc);
            continue;
        }
        struct block_request *req = btpd_calloc(1, sizeof(*req));
        req->blk = &pc->blocks[pc->next_block];
        req->p = p;
        BTPDQ_INSERT_TAIL(&pc->blocks[pc->next_block].reqs, req, blk_entry);
        pc->nreqs++;
        INCNEXTBLOCK(pc);
        peer_request(p, req);
    } while (!peer_laden(p) && pc->next_block != first_block);
}

void
dl_assign_requests_eg(struct peer *p)
{
    assert(!peer_laden(p));
    struct torrent *tp = p->tp;
    struct piece_tq tmp;
    BTPDQ_INIT(&tmp);

    struct piece *pc = BTPDQ_FIRST(&tp->getlst);
    while (!peer_laden(p) && pc != NULL) {
        struct piece *next = BTPDQ_NEXT(pc, entry);
        if (peer_has(p, pc->index) && pc->nblocks != pc->ngot) {
            dl_piece_assign_requests_eg(pc, p);
            BTPDQ_REMOVE(&tp->getlst, pc, entry);
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

        pc = req->blk->pc;
        BTPDQ_REMOVE(&pc->tp->getlst, pc, entry);
        BTPDQ_INSERT_HEAD(&tmp, pc, entry);

        while (req != NULL) {
            struct block_request *next = BTPDQ_NEXT(req, p_entry);
            BTPDQ_REMOVE(&p->my_reqs, req, p_entry);
            p->nreqs_out--;
            BTPDQ_REMOVE(&req->blk->reqs, req, blk_entry);
            free(req);
            pc->nreqs--;

            while (next != NULL && next->blk->pc != pc)
                next = BTPDQ_NEXT(next, p_entry);
            req = next;
        }
    }
    assert(BTPDQ_EMPTY(&p->my_reqs));

    pc = BTPDQ_FIRST(&tmp);
    while (pc != NULL) {
        struct piece *next = BTPDQ_NEXT(pc, entry);
        dl_piece_insert_eg(pc);
        pc = next;
    }
}
