#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <math.h>
#include <string.h>
#include <unistd.h>

#include "btpd.h"

unsigned long
peer_get_rate(unsigned long *rates)
{
    unsigned long ret = 0;
    for (int i = 0; i < RATEHISTORY; i++)
	ret += rates[i];
    return ret;
}

void
peer_kill(struct peer *p)
{
    struct nb_link *nl;

    btpd_log(BTPD_L_CONN, "killed peer %p\n", p);

    if (p->flags & PF_ATTACHED)
	cm_on_lost_peer(p);
    else
	BTPDQ_REMOVE(&btpd.unattached, p, cm_entry);
    if (p->flags & PF_ON_READQ)
	BTPDQ_REMOVE(&btpd.readq, p, rq_entry);
    if (p->flags & PF_ON_WRITEQ)
	BTPDQ_REMOVE(&btpd.writeq, p, wq_entry);

    close(p->sd);
    event_del(&p->in_ev);
    event_del(&p->out_ev);

    nl = BTPDQ_FIRST(&p->outq);
    while (nl != NULL) {
	struct nb_link *next = BTPDQ_NEXT(nl, entry);
	nb_drop(nl->nb);
	free(nl);
	nl = next;
    }

    if (p->net.buf != NULL)
	free(p->net.buf);
    if (p->piece_field != NULL)
        free(p->piece_field);
    free(p);
    btpd.npeers--;
}

void
peer_send(struct peer *p, struct net_buf *nb)
{
    struct nb_link *nl = btpd_calloc(1, sizeof(*nl));
    nl->nb = nb;
    nb_hold(nb);

    if (BTPDQ_EMPTY(&p->outq)) {
	assert(p->outq_off == 0);
	event_add(&p->out_ev, WRITE_TIMEOUT);
    }
    BTPDQ_INSERT_TAIL(&p->outq, nl, entry);
}

/*
 * Remove a network buffer from the peer's outq.
 * If a part of the buffer already have been written
 * to the network it cannot be removed.
 *
 * Returns 1 if the buffer is removed, 0 if not.
 */
int
peer_unsend(struct peer *p, struct nb_link *nl)
{
    if (!(nl == BTPDQ_FIRST(&p->outq) && p->outq_off > 0)) {
	BTPDQ_REMOVE(&p->outq, nl, entry);
	if (nl->nb->type == NB_TORRENTDATA) {
	    assert(p->npiece_msgs > 0);
	    p->npiece_msgs--;
	}
	nb_drop(nl->nb);
	free(nl);
	if (BTPDQ_EMPTY(&p->outq)) {
	    if (p->flags & PF_ON_WRITEQ) {
		BTPDQ_REMOVE(&btpd.writeq, p, wq_entry);
		p->flags &= ~PF_ON_WRITEQ;
	    } else
		event_del(&p->out_ev);
	}
	return 1;
    } else
	return 0;
}

void
peer_sent(struct peer *p, struct net_buf *nb)
{
    switch (nb->type) {
    case NB_CHOKE:
	btpd_log(BTPD_L_MSG, "sent choke to %p\n", p);
	break;
    case NB_UNCHOKE:
	btpd_log(BTPD_L_MSG, "sent unchoke to %p\n", p);
	p->flags &= ~PF_NO_REQUESTS;
	break;
    case NB_INTEREST:
	btpd_log(BTPD_L_MSG, "sent interest to %p\n", p);
	break;
    case NB_UNINTEREST:
	btpd_log(BTPD_L_MSG, "sent uninterest to %p\n", p);
	break;
    case NB_HAVE:
	btpd_log(BTPD_L_MSG, "sent have(%u) to %p\n",
	    nb_get_index(nb), p);
	break;
    case NB_BITFIELD:
	btpd_log(BTPD_L_MSG, "sent bitfield to %p\n", p);
	break;
    case NB_REQUEST:
	btpd_log(BTPD_L_MSG, "sent request(%u,%u,%u) to %p\n",
	    nb_get_index(nb), nb_get_begin(nb), nb_get_length(nb), p);
	break;
    case NB_PIECE:
	btpd_log(BTPD_L_MSG, "sent piece(%u,%u,%u) to %p\n",
	    nb_get_index(nb), nb_get_begin(nb), nb_get_length(nb), p);
	break;
    case NB_CANCEL:
	btpd_log(BTPD_L_MSG, "sent cancel(%u,%u,%u) to %p\n",
	    nb_get_index(nb), nb_get_begin(nb), nb_get_length(nb), p);
	break;
    case NB_TORRENTDATA:
	btpd_log(BTPD_L_MSG, "sent data to %p\n", p);
	assert(p->npiece_msgs > 0);
	p->npiece_msgs--;
	break;
    case NB_MULTIHAVE:
	btpd_log(BTPD_L_MSG, "sent multihave to %p\n", p);
	break;
    case NB_BITDATA:
	btpd_log(BTPD_L_MSG, "sent bitdata to %p\n", p);
	break;
    case NB_SHAKE:
	btpd_log(BTPD_L_MSG, "sent shake to %p\n", p);
	break;
    }
}

void
peer_request(struct peer *p, struct block_request *req)
{
    assert(p->nreqs_out < MAXPIPEDREQUESTS);
    p->nreqs_out++;
    BTPDQ_INSERT_TAIL(&p->my_reqs, req, p_entry);
    peer_send(p, req->blk->msg);
}

int
peer_requested(struct peer *p, struct block *blk)
{
    struct block_request *req;
    BTPDQ_FOREACH(req, &p->my_reqs, p_entry)
	if (req->blk == blk)
	    return 1;
    return 0;
}

void
peer_cancel(struct peer *p, struct block_request *req, struct net_buf *nb)
{
    BTPDQ_REMOVE(&p->my_reqs, req, p_entry);
    p->nreqs_out--;

    int removed = 0;
    struct nb_link *nl;
    BTPDQ_FOREACH(nl, &p->outq, entry) {
	if (nl->nb == req->blk->msg) {
	    removed = peer_unsend(p, nl);
	    break;
	}
    }
    if (!removed)
	peer_send(p, nb);
}

void
peer_unchoke(struct peer *p)
{
    p->flags &= ~PF_I_CHOKE;
    peer_send(p, btpd.unchoke_msg);
}

void
peer_choke(struct peer *p)
{
    struct nb_link *nl = BTPDQ_FIRST(&p->outq);
    while (nl != NULL) {
	struct nb_link *next = BTPDQ_NEXT(nl, entry);
	if (nl->nb->type == NB_PIECE) {
	    struct nb_link *data = next;
	    next = BTPDQ_NEXT(next, entry);
	    if (peer_unsend(p, nl))
		peer_unsend(p, data);
	}
	nl = next;
    }

    p->flags |= PF_I_CHOKE;
    peer_send(p, btpd.choke_msg);
}

void
peer_want(struct peer *p, uint32_t index)
{
    assert(p->nwant < p->npieces);
    p->nwant++;
    if (p->nwant == 1) {
	int unsent = 0;
	struct nb_link *nl = BTPDQ_LAST(&p->outq, nb_tq);
	if (nl != NULL && nl->nb->type == NB_UNINTEREST)
	    unsent = peer_unsend(p, nl);
	if (!unsent)
	    peer_send(p, btpd.interest_msg);
	p->flags |= PF_I_WANT;
    }
}

void
peer_unwant(struct peer *p, uint32_t index)
{
    assert(p->nwant > 0);
    p->nwant--;
    if (p->nwant == 0) {
	p->flags &= ~PF_I_WANT;
	peer_send(p, btpd.uninterest_msg);
    }
}

static struct peer *
peer_create_common(int sd)
{
    struct peer *p = btpd_calloc(1, sizeof(*p));

    p->sd = sd;
    p->flags = PF_I_CHOKE | PF_P_CHOKE;
    BTPDQ_INIT(&p->my_reqs);
    BTPDQ_INIT(&p->outq);

    net_set_state(p, SHAKE_PSTR, 28);

    event_set(&p->out_ev, p->sd, EV_WRITE, net_write_cb, p);
    event_set(&p->in_ev, p->sd, EV_READ, net_read_cb, p);
    event_add(&p->in_ev, NULL);

    BTPDQ_INSERT_TAIL(&btpd.unattached, p, cm_entry);
    btpd.npeers++;
    return p;
}

void
peer_create_in(int sd)
{
    struct peer *p = peer_create_common(sd);
    p->flags |= PF_INCOMING;
}

void
peer_create_out(struct torrent *tp, const uint8_t *id,
    const char *ip, int port)
{
    int sd;
    struct peer *p;

    if (net_connect(ip, port, &sd) != 0)
	return;

    p = peer_create_common(sd);
    p->tp = tp;
    peer_send(p, nb_create_shake(p->tp));
}

void
peer_create_out_compact(struct torrent *tp, const char *compact)
{
    int sd;
    struct peer *p;
    struct sockaddr_in addr;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = *(long *)compact;
    addr.sin_port = *(short *)(compact + 4);

    if (net_connect2((struct sockaddr *)&addr, sizeof(addr), &sd) != 0)
	return;

    p = peer_create_common(sd);
    p->tp = tp;
    peer_send(p, nb_create_shake(p->tp));
}

void
peer_on_shake(struct peer *p)
{
    btpd_log(BTPD_L_MSG, "received shake from %p.\n", p);
    p->piece_field = btpd_calloc(1, (int)ceil(p->tp->meta.npieces / 8.0));
    if (p->tp->have_npieces > 0) {
        if (p->tp->have_npieces * 9 < 5 + ceil(p->tp->meta.npieces / 8.0))
            peer_send(p, nb_create_multihave(p->tp));
        else {
            peer_send(p, nb_create_bitfield(p->tp));
            peer_send(p, nb_create_bitdata(p->tp));
        }
    }
    cm_on_new_peer(p);
}

void
peer_on_choke(struct peer *p)
{
    btpd_log(BTPD_L_MSG, "received choke from %p\n", p);
    if ((p->flags & PF_P_CHOKE) != 0)
	return;
    else {
	p->flags |= PF_P_CHOKE;
	cm_on_choke(p);
    }
}

void
peer_on_unchoke(struct peer *p)
{
    btpd_log(BTPD_L_MSG, "received unchoke from %p\n", p);
    if ((p->flags & PF_P_CHOKE) == 0)
	return;
    else {
	p->flags &= ~PF_P_CHOKE;
	cm_on_unchoke(p);
    }
}

void
peer_on_interest(struct peer *p)
{
    btpd_log(BTPD_L_MSG, "received interest from %p\n", p);
    if ((p->flags & PF_P_WANT) != 0)
	return;
    else {
	p->flags |= PF_P_WANT;
	cm_on_interest(p);
    }
}

void
peer_on_uninterest(struct peer *p)
{
    btpd_log(BTPD_L_MSG, "received uninterest from %p\n", p);
    if ((p->flags & PF_P_WANT) == 0)
	return;
    else {
	p->flags &= ~PF_P_WANT;
	cm_on_uninterest(p);
    }
}

void
peer_on_have(struct peer *p, uint32_t index)
{
    btpd_log(BTPD_L_MSG, "received have(%u) from %p\n", index, p);
    if (!has_bit(p->piece_field, index)) {
	set_bit(p->piece_field, index);
	p->npieces++;
	cm_on_piece_ann(p, index);
    }
}

void
peer_on_bitfield(struct peer *p, const uint8_t *field)
{
    btpd_log(BTPD_L_MSG, "received bitfield from %p\n", p);
    assert(p->npieces == 0);
    bcopy(field, p->piece_field, (size_t)ceil(p->tp->meta.npieces / 8.0));
    for (uint32_t i = 0; i < p->tp->meta.npieces; i++) {
	if (has_bit(p->piece_field, i)) {
	    p->npieces++;
	    cm_on_piece_ann(p, i);
	}
    }
}

void
peer_on_piece(struct peer *p, uint32_t index, uint32_t begin,
    uint32_t length, const char *data)
{
    btpd_log(BTPD_L_MSG, "received piece(%u,%u,%u) from %p\n",
	index, begin, length, p);
    struct block_request *req = BTPDQ_FIRST(&p->my_reqs);
    if (req == NULL)
	return;
    struct net_buf *nb = req->blk->msg;
    if (nb_get_begin(nb) == begin &&
	nb_get_index(nb) == index &&
	nb_get_length(nb) == length) {

	assert(p->nreqs_out > 0);
	p->nreqs_out--;
	BTPDQ_REMOVE(&p->my_reqs, req, p_entry);
	cm_on_block(p, req, index, begin, length, data);
    }
}

void
peer_on_request(struct peer *p, uint32_t index, uint32_t begin,
    uint32_t length)
{
    btpd_log(BTPD_L_MSG, "received request(%u,%u,%u) from %p\n",
	index, begin, length, p);
    if ((p->flags & PF_NO_REQUESTS) == 0) {
	off_t cbegin = index * p->tp->meta.piece_length + begin;
	char * content = torrent_get_bytes(p->tp, cbegin, length);
	peer_send(p, nb_create_piece(index, begin, length));
	peer_send(p, nb_create_torrentdata(content, length));
	p->npiece_msgs++;
	if (p->npiece_msgs >= MAXPIECEMSGS) {
	    peer_send(p, btpd.choke_msg);
	    peer_send(p, btpd.unchoke_msg);
	    p->flags |= PF_NO_REQUESTS;
	}
    }
}

void
peer_on_cancel(struct peer *p, uint32_t index, uint32_t begin,
    uint32_t length)
{
    btpd_log(BTPD_L_MSG, "received cancel(%u,%u,%u) from %p\n",
	index, begin, length, p);
    struct nb_link *nl;
    BTPDQ_FOREACH(nl, &p->outq, entry)
	if (nl->nb->type == NB_PIECE
	    && nb_get_begin(nl->nb) == begin
	    && nb_get_index(nl->nb) == index
	    && nb_get_length(nl->nb) == length) {
	    struct nb_link *data = BTPDQ_NEXT(nl, entry);
	    if (peer_unsend(p, nl))
		peer_unsend(p, data);
	    break;
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
