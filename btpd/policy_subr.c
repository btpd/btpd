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

static int
cm_should_enter_endgame(struct torrent *tp)
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
cm_enter_endgame(struct torrent *tp)
{
    struct peer *p;
    btpd_log(BTPD_L_POL, "Entering end game\n");
    tp->endgame = 1;
    BTPDQ_FOREACH(p, &tp->peers, cm_entry) {
	struct piece *pc;
	BTPDQ_FOREACH(pc, &tp->getlst, entry) {
	    if (peer_has(p, pc->index)) {
		peer_want(p, pc->index);
		if (peer_leech_ok(p))
		    cm_piece_assign_requests_eg(pc, p);
	    }
	}
    }
}

int
peer_chokes(struct peer *p)
{
    return p->flags & PF_P_CHOKE;
}

int
peer_has(struct peer *p, uint32_t index)
{
    return has_bit(p->piece_field, index);
}

int
peer_laden(struct peer *p)
{
    return p->nreqs_out >= MAXPIPEDREQUESTS;
}

int
peer_wanted(struct peer *p)
{
    return (p->flags & PF_I_WANT) == PF_I_WANT;
}

int
peer_leech_ok(struct peer *p)
{
    return (p->flags & (PF_I_WANT|PF_P_CHOKE)) == PF_I_WANT;
}

int
piece_full(struct piece *pc)
{
    return pc->ngot + pc->nbusy == pc->nblocks;
}

struct piece *
torrent_get_piece(struct torrent *tp, uint32_t index)
{
    struct piece *pc;
    BTPDQ_FOREACH(pc, &tp->getlst, entry)
	if (pc->index == index)
	    break;
    return pc;
}

static struct piece *
piece_alloc(struct torrent *tp, uint32_t index)
{
    assert(!has_bit(tp->busy_field, index)
	&& tp->npcs_busy < tp->meta.npieces);
    struct piece *pc;
    size_t mem, field;
    unsigned nblocks;
    off_t piece_length = torrent_piece_size(tp, index);

    nblocks = (unsigned)ceil((double)piece_length / PIECE_BLOCKLEN);
    field = (size_t)ceil(nblocks / 8.0);
    mem = sizeof(*pc) + field;

    pc = btpd_calloc(1, mem);
    pc->tp = tp;
    pc->down_field = (uint8_t *)(pc + 1);
    pc->have_field =
	tp->block_field +
	(size_t)ceil(index * tp->meta.piece_length / (double)(1 << 17));
    pc->nblocks = nblocks;
    pc->index = index;

    for (unsigned i = 0; i < nblocks; i++)
	if (has_bit(pc->have_field, i))
	    pc->ngot++;

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
    free(pc);
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

	err = vopen(&fd, O_RDONLY, "%s", tp->relpath);
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
    return vopen(fd, O_RDONLY, "%s.d/%s", tp->relpath, path);
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
	cm_on_ok_piece(pc);
    else
	cm_on_bad_piece(pc);
}

void
cm_on_piece(struct piece *pc)
{
    torrent_test_piece(pc);
}

static int
cm_piece_startable(struct peer *p, uint32_t index)
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
cm_choose_rarest(struct peer *p, uint32_t *res)
{
    uint32_t i;
    struct torrent *tp = p->tp;

    assert(tp->endgame == 0);

    for (i = 0; i < tp->meta.npieces && !cm_piece_startable(p, i); i++)
	;

    if (i == tp->meta.npieces)
	return ENOENT;
    
    uint32_t min_i = i;
    uint32_t min_c = 1;
    for(i++; i < tp->meta.npieces; i++) {
	if (cm_piece_startable(p, i)) {
	    if (tp->piece_count[i] == tp->piece_count[min_i])
		min_c++;
	    else if (tp->piece_count[i] < tp->piece_count[min_i]) {
		min_i = i;
		min_c = 1;
	    }
	}
    }
    if (min_c > 1) {
	min_c = 1 + rint((double)random() * (min_c - 1) / RAND_MAX);
	for (i = min_i; min_c > 0; i++) {
	    if (cm_piece_startable(p, i)
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
 * Allocate the piece indicated by the index for download.
 * There's a small possibility that a piece is fully downloaded
 * but haven't been tested. If such is the case the piece will
 * be tested and NULL will be returned. Also, we might then enter
 * end game.
 *
 * Return the piece or NULL.
 */
struct piece *
cm_new_piece(struct torrent *tp, uint32_t index)
{
    btpd_log(BTPD_L_POL, "Started on piece %u.\n", index);
    struct piece *pc = piece_alloc(tp, index);
    if (pc->ngot == pc->nblocks) {
	cm_on_piece(pc);
	if (cm_should_enter_endgame(tp))
	    cm_enter_endgame(tp);
	return NULL;
    } else
	return pc;
}

/*
 * Called from either cm_piece_assign_requests or cm_new_piece, 
 * when a pice becomes full. The wanted level of the peers
 * that has this piece will be decreased. This function is
 * the only one that may trigger end game.
 */
static void
cm_on_piece_full(struct piece *pc)
{
    struct peer *p;
    BTPDQ_FOREACH(p, &pc->tp->peers, cm_entry) {
	if (peer_has(p, pc->index))
	    peer_unwant(p, pc->index);
    }
    if (cm_should_enter_endgame(pc->tp))
	cm_enter_endgame(pc->tp);
}


/*
 * Called when a previously full piece loses a peer.
 * This is needed because we have decreased the wanted
 * level for the peers that have this piece when it got
 * full. Thus we have to increase the wanted level and
 * try to assign requests for this piece.
 */
void
cm_on_piece_unfull(struct piece *pc)
{
    struct torrent *tp = pc->tp;
    struct peer *p;
    assert(!piece_full(pc) && tp->endgame == 0);
    BTPDQ_FOREACH(p, &tp->peers, cm_entry)
	if (peer_has(p, pc->index))
	    peer_want(p, pc->index);
    p = BTPDQ_FIRST(&tp->peers);
    while (p != NULL && !piece_full(pc)) {
	if (peer_leech_ok(p) && !peer_laden(p))
	    cm_piece_assign_requests(pc, p); // Cannot provoke end game here.
	p = BTPDQ_NEXT(p, cm_entry);
    }
}

/*
 * Request as many blocks as possible on this piece from
 * the peer. If the piece becomes full we call cm_on_piece_full.
 *
 * Return the number of requests sent.
 */
unsigned
cm_piece_assign_requests(struct piece *pc, struct peer *p)
{
    assert(!piece_full(pc) && !peer_laden(p));
    unsigned count = 0;
    for (uint32_t i = 0; !piece_full(pc) && !peer_laden(p); i++) {
	if (has_bit(pc->have_field, i) || has_bit(pc->down_field, i))
	    continue;
	set_bit(pc->down_field, i);
	pc->nbusy++;
	uint32_t start = i * PIECE_BLOCKLEN;
	uint32_t len = torrent_block_size(pc, i);
	peer_request(p, pc->index, start, len);
	count++;
    }
    if (piece_full(pc))
	cm_on_piece_full(pc);
    return count;
}

/*
 * Request as many blocks as possible from the peer. Puts
 * requests on already active pieces before starting on new
 * ones. Care must be taken since end game mode may be triggered
 * by the calls to cm_piece_assign_requests.
 *
 * Returns number of requests sent.
 *
 * XXX: should do something smart when deciding on which
 *      already started piece to put requests on.
 */
unsigned
cm_assign_requests(struct peer *p)
{
    assert(!p->tp->endgame);
    struct piece *pc;
    struct torrent *tp = p->tp;
    unsigned count = 0;
    BTPDQ_FOREACH(pc, &tp->getlst, entry) {
	if (piece_full(pc) || !peer_has(p, pc->index))
	    continue;
	count += cm_piece_assign_requests(pc, p);
	if (tp->endgame)
	    break;
	if (!piece_full(pc))
	    assert(peer_laden(p));
	if (peer_laden(p))
	    break;
    }
    while (!peer_laden(p) && !tp->endgame) {
	uint32_t index;
	if (cm_choose_rarest(p, &index) == 0) {
	    pc = cm_new_piece(tp, index);
	    if (pc != NULL)
		count += cm_piece_assign_requests(pc, p);
	} else
	    break;
    }
    return count;
}

void
cm_unassign_requests(struct peer *p)
{
    struct torrent *tp = p->tp;

    struct piece *pc = BTPDQ_FIRST(&tp->getlst);
    while (pc != NULL) {
	int was_full = piece_full(pc);

	struct piece_req *req = BTPDQ_FIRST(&p->my_reqs);
	while (req != NULL) {
	    struct piece_req *next = BTPDQ_NEXT(req, entry);

	    if (pc->index == req->index) {
		// XXX: Needs to be looked at if we introduce snubbing.
		assert(has_bit(pc->down_field, req->begin / PIECE_BLOCKLEN));
		clear_bit(pc->down_field, req->begin / PIECE_BLOCKLEN);
		pc->nbusy--;
		BTPDQ_REMOVE(&p->my_reqs, req, entry);
		free(req);
	    }
	    
	    req = next;
	}
	
	if (was_full && !piece_full(pc))
	    cm_on_piece_unfull(pc);

	pc = BTPDQ_NEXT(pc, entry);
    }

    assert(BTPDQ_EMPTY(&p->my_reqs));
}


void
cm_piece_assign_requests_eg(struct piece *pc, struct peer *p)
{
    for (uint32_t i = 0; i < pc->nblocks; i++) {
	if (!has_bit(pc->have_field, i)) {
	    uint32_t start = i * PIECE_BLOCKLEN;
	    uint32_t len = torrent_block_size(pc, i);
	    peer_request(p, pc->index, start, len);
	}
    }
}

void
cm_assign_requests_eg(struct peer *p)
{
    struct torrent *tp = p->tp;
    struct piece *pc;
    BTPDQ_FOREACH(pc, &tp->getlst, entry) {
	if (peer_has(p, pc->index))
	    cm_piece_assign_requests_eg(pc, p);
    }
}

void
cm_unassign_requests_eg(struct peer *p)
{
    struct piece_req *req = BTPDQ_FIRST(&p->my_reqs);
    while (req != NULL) {
	struct piece_req *next = BTPDQ_NEXT(req, entry);
	free(req);
	req = next;
    }
    BTPDQ_INIT(&p->my_reqs);
    p->nreqs_out = 0;
}
