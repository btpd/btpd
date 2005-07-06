#include <sys/types.h>
#include <sys/mman.h>

#include <openssl/sha.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "btpd.h"
#include "stream.h"
#include "tracker_req.h"

#define BLOCKLEN (1 << 14)

static void cm_on_piece(struct torrent *tp, struct piece *piece);

static void
assign_piece_requests_eg(struct piece *piece, struct peer *peer)
{
    for (unsigned i = 0; i < piece->nblocks; i++) {
	if (!has_bit(piece->have_field, i)) {
	    uint32_t start = i * BLOCKLEN;
	    uint32_t len;
	    if (i < piece->nblocks - 1)
		len = BLOCKLEN;
	    else if (piece->index < peer->tp->meta.npieces - 1)
		len = peer->tp->meta.piece_length - i * BLOCKLEN;
	    else {
		off_t piece_len =
		    peer->tp->meta.total_length -
		    peer->tp->meta.piece_length *
		    (peer->tp->meta.npieces - 1);
		len = piece_len - i * BLOCKLEN;
	    }
	    peer_request(peer, piece->index, start, len);
	}
    }
}

static void
cm_assign_requests_eg(struct peer *peer)
{
    struct piece *piece;
    BTPDQ_FOREACH(piece, &peer->tp->getlst, entry) {
	if (has_bit(peer->piece_field, piece->index)) {
	    peer_want(peer, piece->index);
	    if ((peer->flags & PF_P_CHOKE) == 0)
		assign_piece_requests_eg(piece, peer);
	}
    }
}

static void
cm_unassign_requests_eg(struct peer *peer)
{
    struct piece_req *req = BTPDQ_FIRST(&peer->my_reqs);
    while (req != NULL) {
	struct piece_req *next = BTPDQ_NEXT(req, entry);
	free(req);
	req = next;
    }
    BTPDQ_INIT(&peer->my_reqs);
}

static void
cm_enter_endgame(struct torrent *tp)
{
    struct peer *peer;
    btpd_log(BTPD_L_POL, "Entering end game\n");
    tp->endgame = 1;
    BTPDQ_FOREACH(peer, &tp->peers, cm_entry)
	cm_assign_requests_eg(peer);
}

static int
piece_full(struct piece *p)
{
    return p->ngot + p->nbusy == p->nblocks;
}

static int
cm_should_schedule(struct torrent *tp)
{
    if (!tp->endgame) {
	int should = 1;
	struct piece *p = BTPDQ_FIRST(&tp->getlst);
	while (p != NULL) {
	    if (!piece_full(p)) {
		should = 0;
		break;
	    }
	    p = BTPDQ_NEXT(p, entry);
	}
	return should;
    } else
	return 0;
}

static void
cm_on_peerless_piece(struct torrent *tp, struct piece *piece)
{
    if (!tp->endgame) {
	assert(tp->piece_count[piece->index] == 0);
	btpd_log(BTPD_L_POL, "peerless piece %u\n", piece->index);
	msync(tp->imem, tp->isiz, MS_ASYNC);
	BTPDQ_REMOVE(&tp->getlst, piece, entry);
	free(piece);
	if (cm_should_schedule(tp))
	    cm_schedule_piece(tp);
    }
}

static int
rate_cmp(unsigned long rate1, unsigned long rate2)
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
    unsigned long rate1 = peer_get_rate((*(struct peer **)p1)->rate_to_me);
    unsigned long rate2 = peer_get_rate((*(struct peer **)p2)->rate_to_me);
    return rate_cmp(rate1, rate2);
}

static int
uprate_cmp(const void *p1, const void *p2)
{
    unsigned long rate1 = peer_get_rate((*(struct peer **)p1)->rate_from_me);
    unsigned long rate2 = peer_get_rate((*(struct peer **)p2)->rate_from_me);
    return rate_cmp(rate1, rate2);
}

static void
choke_alg(struct torrent *tp)
{
    int i;
    struct peer *p;
    struct peer **psort;

    assert(tp->npeers > 0);

    psort = (struct peer **)btpd_malloc(tp->npeers * sizeof(p));
    i = 0;
    BTPDQ_FOREACH(p, &tp->peers, cm_entry)
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
    free(psort);

    tp->choke_time = btpd.seconds + 10;
}

static void
next_optimistic(struct torrent *tp, struct peer *np)
{
    if (np != NULL)
	tp->optimistic = np;
    else if (tp->optimistic == NULL)
	tp->optimistic = BTPDQ_FIRST(&tp->peers);
    else {
	np = BTPDQ_NEXT(tp->optimistic, cm_entry);
	if (np != NULL)
	    tp->optimistic = np;
	else
	    tp->optimistic = BTPDQ_FIRST(&tp->peers);
    }
    assert(tp->optimistic != NULL);
    choke_alg(tp);
    tp->opt_time = btpd.seconds + 30;
}

void
cm_on_upload(struct peer *peer)
{
    choke_alg(peer->tp);
}

void
cm_on_unupload(struct peer *peer)
{
    choke_alg(peer->tp);
}

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

void
cm_on_download(struct peer *peer)
{
    if (!peer->tp->endgame)
	assert(cm_assign_requests(peer, 5) != 0);
    else
	cm_assign_requests_eg(peer);
}

void
cm_on_undownload(struct peer *peer)
{
    if (!peer->tp->endgame)
	cm_unassign_requests(peer);
    else
	cm_unassign_requests_eg(peer);
}

void
cm_on_piece_ann(struct peer *peer, uint32_t piece)
{
    struct piece *p;
    struct torrent *tp = peer->tp;

    tp->piece_count[piece]++;

    if (has_bit(tp->piece_field, piece))
	return;

    p = BTPDQ_FIRST(&tp->getlst);
    while (p != NULL && p->index != piece)
	p = BTPDQ_NEXT(p, entry);

    if (p != NULL && tp->endgame) {
	peer_want(peer, p->index);
	if ((peer->flags & PF_P_CHOKE) == 0)
	    cm_on_download(peer);
    } else if (p != NULL && !piece_full(p)) {
	peer_want(peer, p->index);
	if ((peer->flags & PF_P_CHOKE) == 0 && BTPDQ_EMPTY(&peer->my_reqs))
	    cm_on_download(peer);
    } else if (p == NULL && cm_should_schedule(tp))
	cm_schedule_piece(tp);
}

void
cm_on_lost_peer(struct peer *peer)
{
    struct torrent *tp = peer->tp;
    struct piece *piece;

    tp->npeers--;
    peer->flags &= ~PF_ATTACHED;
    if (tp->npeers == 0) {
	BTPDQ_REMOVE(&tp->peers, peer, cm_entry);
	tp->optimistic = NULL;
	tp->choke_time = tp->opt_time = 0;
    } else if (tp->optimistic == peer) {
	struct peer *next = BTPDQ_NEXT(peer, cm_entry);
	BTPDQ_REMOVE(&tp->peers, peer, cm_entry);
	next_optimistic(peer->tp, next);
    } else if ((peer->flags & (PF_P_WANT|PF_I_CHOKE)) == PF_P_WANT) {
	BTPDQ_REMOVE(&tp->peers, peer, cm_entry);
	cm_on_unupload(peer);
    } else {
	BTPDQ_REMOVE(&tp->peers, peer, cm_entry);
    }

    for (size_t i = 0; i < peer->tp->meta.npieces; i++)
	if (has_bit(peer->piece_field, i))
	    tp->piece_count[i]--;

    if ((peer->flags & (PF_I_WANT|PF_P_CHOKE)) == PF_I_WANT)
	cm_on_undownload(peer);

    piece = BTPDQ_FIRST(&tp->getlst);
    while (piece != NULL) {
	struct piece *next = BTPDQ_NEXT(piece, entry);
	if (has_bit(peer->piece_field, piece->index) &&
	    tp->piece_count[piece->index] == 0)
	    cm_on_peerless_piece(tp, piece);
	piece = next;
    }
}

void
cm_on_new_peer(struct peer *peer)
{
    struct torrent *tp = peer->tp;

    tp->npeers++;
    peer->flags |= PF_ATTACHED;
    BTPDQ_REMOVE(&btpd.unattached, peer, cm_entry);

    if (tp->npeers == 1) {
	BTPDQ_INSERT_HEAD(&tp->peers, peer, cm_entry);
	next_optimistic(peer->tp, peer);
    } else {
	if (random() > RAND_MAX / 3)
	    BTPDQ_INSERT_AFTER(&tp->peers, tp->optimistic, peer, cm_entry);
	else
	    BTPDQ_INSERT_TAIL(&tp->peers, peer, cm_entry);
    }
}

static int
missing_piece(struct torrent *tp, uint32_t index)
{
    struct piece *p;
    if (has_bit(tp->piece_field, index))
	return 0;
    BTPDQ_FOREACH(p, &tp->getlst, entry)
	if (p->index == index)
	    return 0;
    return 1;
}

static struct piece *
alloc_piece(struct torrent *tp, uint32_t piece)
{
    struct piece *res;
    size_t mem, field;
    unsigned long nblocks;
    off_t piece_length = tp->meta.piece_length;

    if (piece == tp->meta.npieces - 1) {
	off_t totl = tp->meta.total_length;
	off_t npm1 = tp->meta.npieces - 1;
	piece_length = totl - npm1 * piece_length;
    }

    nblocks = (unsigned)ceil((double)piece_length / BLOCKLEN);
    field = (size_t)ceil(nblocks / 8.0);
    mem = sizeof(*res) + field;

    res = btpd_calloc(1, mem);
    res->down_field = (uint8_t *)res + sizeof(*res);
    res->have_field =
	tp->block_field +
	(size_t)ceil(piece * tp->meta.piece_length / (double)(1 << 17));
    res->nblocks = nblocks;
    res->index = piece;

    for (unsigned i = 0; i < nblocks; i++)
	if (has_bit(res->have_field, i))
	    res->ngot++;

    return res;
}

static void
activate_piece_peers(struct torrent *tp, struct piece *piece)
{
    struct peer *peer;
    assert(!piece_full(piece) && tp->endgame == 0);
    BTPDQ_FOREACH(peer, &tp->peers, cm_entry)
	if (has_bit(peer->piece_field, piece->index))
	    peer_want(peer, piece->index);
    peer = BTPDQ_FIRST(&tp->peers);
    while (peer != NULL && !piece_full(piece)) {
	if ((peer->flags & (PF_P_CHOKE|PF_I_WANT)) == PF_I_WANT &&
	    BTPDQ_EMPTY(&peer->my_reqs)) {
	    //
	    cm_on_download(peer);
	}
	peer = BTPDQ_NEXT(peer, cm_entry);
    }
}

void
cm_schedule_piece(struct torrent *tp)
{
    uint32_t i;
    uint32_t min_i;
    unsigned min_c;
    struct piece *piece;
    int enter_end_game = 1;

    assert(tp->endgame == 0);

    for (i = 0; i < tp->meta.npieces; i++)
	if (missing_piece(tp, i)) {
	    enter_end_game = 0;
	    if (tp->piece_count[i] > 0)
		break;
	}

    if (i == tp->meta.npieces) {
	if (enter_end_game)
	    cm_enter_endgame(tp);
	return;
    }

    min_i = i;
    min_c = 1;
    for(i++; i < tp->meta.npieces; i++) {
	if (missing_piece(tp, i) && tp->piece_count[i] > 0) {
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
	    if (missing_piece(tp, i) &&
		tp->piece_count[i] == tp->piece_count[min_i]) {
		//
		min_c--;
		min_i = i;
	    }
	}
    }

    btpd_log(BTPD_L_POL, "scheduled piece: %u.\n", min_i);
    piece = alloc_piece(tp, min_i);
    BTPDQ_INSERT_HEAD(&tp->getlst, piece, entry);
    if (piece->ngot == piece->nblocks) {
	cm_on_piece(tp, piece);
	if (cm_should_schedule(tp))
	    cm_schedule_piece(tp);
    } else
	activate_piece_peers(tp, piece);
}

static void
cm_on_piece_unfull(struct torrent *tp, struct piece *piece)
{
    activate_piece_peers(tp, piece);
}

static void
cm_on_piece_full(struct torrent *tp, struct piece *piece)
{
    struct peer *p;

    if (cm_should_schedule(tp))
	cm_schedule_piece(tp);
    BTPDQ_FOREACH(p, &tp->peers, cm_entry) {
	if (has_bit(p->piece_field, piece->index))
	    peer_unwant(p, piece->index);
    }
}

static int
cm_assign_request(struct peer *peer)
{
    struct piece *piece;
    unsigned i;
    uint32_t start, len;

    piece = BTPDQ_FIRST(&peer->tp->getlst);
    while (piece != NULL) {
	if (!piece_full(piece) && has_bit(peer->piece_field, piece->index))
	    break;
	piece = BTPDQ_NEXT(piece, entry);
    }

    if (piece == NULL)
	return 0;

    i = 0;
    while(has_bit(piece->have_field, i) || has_bit(piece->down_field, i))
	i++;

    start = i * BLOCKLEN;

    if (i < piece->nblocks - 1)
	len = BLOCKLEN;
    else if (piece->index < peer->tp->meta.npieces - 1)
	len = peer->tp->meta.piece_length - i * BLOCKLEN;
    else {
	off_t piece_len =
	    peer->tp->meta.total_length -
	    peer->tp->meta.piece_length * (peer->tp->meta.npieces - 1);
	len = piece_len - i * BLOCKLEN;
    }

    peer_request(peer, piece->index, start, len);
    set_bit(piece->down_field, i);
    piece->nbusy++;
    
    if (piece_full(piece))
	cm_on_piece_full(peer->tp, piece);

    return 1;
}

int
cm_assign_requests(struct peer *peer, int nreqs)
{
    int onreqs = nreqs;

    while (nreqs > 0 && cm_assign_request(peer))
	nreqs--;

    return onreqs - nreqs;
}

void
cm_unassign_requests(struct peer *peer)
{
    struct torrent *tp = peer->tp;
    struct piece *piece = BTPDQ_FIRST(&tp->getlst);

    while (piece != NULL) {
	int was_full = piece_full(piece);

	struct piece_req *req = BTPDQ_FIRST(&peer->my_reqs);
	while (req != NULL) {
	    struct piece_req *next = BTPDQ_NEXT(req, entry);

	    if (piece->index == req->index) {
		assert(has_bit(piece->down_field, req->begin / BLOCKLEN));
		clear_bit(piece->down_field, req->begin / BLOCKLEN);
		piece->nbusy--;
		BTPDQ_REMOVE(&peer->my_reqs, req, entry);
		free(req);
	    }
	    
	    req = next;
	}
	
	if (was_full && !piece_full(piece))
	    cm_on_piece_unfull(tp, piece);

	piece = BTPDQ_NEXT(piece, entry);
    }

    assert(BTPDQ_EMPTY(&peer->my_reqs));
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
cm_on_piece(struct torrent *tp, struct piece *piece)
{
    int err;
    uint8_t hash[20];
    struct bt_stream_ro *bts;
    off_t plen = tp->meta.piece_length;
    if (piece->index == tp->meta.npieces - 1) {
	plen =
	    tp->meta.total_length -
	    tp->meta.piece_length * (tp->meta.npieces - 1);
    }
    if ((bts = bts_open_ro(&tp->meta, piece->index * tp->meta.piece_length,
			   ro_fd_cb, tp)) == NULL)
	btpd_err("Out of memory.\n");


    if ((err = bts_sha(bts, plen, hash)) != 0)
	btpd_err("Ouch! %s\n", strerror(err));

    bts_close_ro(bts);

    if (test_hash(tp, hash, piece->index) == 0) {
	btpd_log(BTPD_L_POL, "Got piece: %u.\n", piece->index);
	struct peer *p;
	set_bit(tp->piece_field, piece->index);
	tp->have_npieces++;
	if (tp->have_npieces == tp->meta.npieces) {
	    btpd_log(BTPD_L_BTPD, "Finished: %s.\n", tp->relpath);
	    tracker_req(tp, TR_COMPLETED);
	}
	msync(tp->imem, tp->isiz, MS_ASYNC);
	BTPDQ_FOREACH(p, &tp->peers, cm_entry)
	    peer_have(p, piece->index);
	if (tp->endgame)
	    BTPDQ_FOREACH(p, &tp->peers, cm_entry)
		peer_unwant(p, piece->index);
	BTPDQ_REMOVE(&tp->getlst, piece, entry);
	free(piece);
    } else if (tp->endgame) {
	struct peer *p;
	btpd_log(BTPD_L_ERROR, "Bad hash for piece %u of %s.\n",
	    piece->index, tp->relpath);
	for (unsigned i = 0; i < piece->nblocks; i++)
	    clear_bit(piece->have_field, i);
	piece->ngot = 0;
	BTPDQ_FOREACH(p, &tp->peers, cm_entry)
	    if (has_bit(p->piece_field, piece->index) &&
		(p->flags & PF_P_CHOKE) == 0) {
		//
		assign_piece_requests_eg(piece, p);
	    }
    } else {
	btpd_log(BTPD_L_ERROR, "Bad hash for piece %u of %s.\n",
	    piece->index, tp->relpath);
	for (unsigned i = 0; i < piece->nblocks; i++) {
	    clear_bit(piece->have_field, i);
	    assert(!has_bit(piece->down_field, i));
	}
	msync(tp->imem, tp->isiz, MS_ASYNC);
	BTPDQ_REMOVE(&tp->getlst, piece, entry);
	free(piece);
	if (cm_should_schedule(tp))
	    cm_schedule_piece(tp);
    }
}

void
cm_on_block(struct peer *peer)
{
    struct torrent *tp = peer->tp;
    struct piece_req *req = BTPDQ_FIRST(&peer->my_reqs);
    struct piece *piece = BTPDQ_FIRST(&tp->getlst);
    unsigned block = req->begin / BLOCKLEN;
    while (piece != NULL && piece->index != req->index)
        piece = BTPDQ_NEXT(piece, entry);
    set_bit(piece->have_field, block);
    clear_bit(piece->down_field, block);
    piece->ngot++;
    piece->nbusy--;
    if (tp->endgame) {
	uint32_t index = req->index;
	uint32_t begin = req->begin;
	uint32_t length = req->length;
	struct peer *p;

	BTPDQ_REMOVE(&peer->my_reqs, req, entry);
	free(req);

	BTPDQ_FOREACH(p, &tp->peers, cm_entry) {
	    if (has_bit(p->piece_field, index) &&
		(peer->flags & PF_P_CHOKE) == 0)
		peer_cancel(p, index, begin, length);
	}
	if (piece->ngot == piece->nblocks)
	    cm_on_piece(tp, piece);
    } else {
	BTPDQ_REMOVE(&peer->my_reqs, req, entry);
	free(req);
	if (piece->ngot == piece->nblocks)
	    cm_on_piece(tp, piece);
	if ((peer->flags & (PF_I_WANT|PF_P_CHOKE)) == PF_I_WANT)
	    cm_assign_requests(peer, 1);
    }
}
