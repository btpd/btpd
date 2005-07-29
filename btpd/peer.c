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
    struct piece_req *req;

    btpd_log(BTPD_L_CONN, "killed peer.\n");

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
    req = BTPDQ_FIRST(&p->my_reqs);
    while (req != NULL) {
	struct piece_req *next = BTPDQ_NEXT(req, entry);
	free(req);
	req = next;
    }

    p->reader->kill(p->reader);
    if (p->piece_field != NULL)
        free(p->piece_field);
    free(p);
    btpd.npeers--;
}

void
peer_request(struct peer *p, uint32_t index, uint32_t begin, uint32_t len)
{
    p->nreqs_out++;
    struct piece_req *req = btpd_calloc(1, sizeof(*req));
    req->index = index;
    req->begin = begin;
    req->length = len;
    BTPDQ_INSERT_TAIL(&p->my_reqs, req, entry);
    net_send_request(p, req);
}

void
peer_cancel(struct peer *p, uint32_t index, uint32_t begin, uint32_t len)
{
    struct piece_req *req;
again:
    req = BTPDQ_FIRST(&p->my_reqs);
    while (req != NULL &&
	   !(index == req->index &&
	     begin == req->begin &&
	     len == req->length))
	req = BTPDQ_NEXT(req, entry);
    if (req != NULL) {
	net_send_cancel(p, req);
	BTPDQ_REMOVE(&p->my_reqs, req, entry);
	free(req);
	p->nreqs_out--;
	goto again;
    }
}

void
peer_have(struct peer *p, uint32_t index)
{
    net_send_have(p, index);
}

void
peer_unchoke(struct peer *p)
{
    p->flags &= ~PF_I_CHOKE;
    net_send_unchoke(p);
}

void
peer_choke(struct peer *p)
{
    struct nb_link *nl = BTPDQ_FIRST(&p->outq);
    while (nl != NULL) {
	struct nb_link *next = BTPDQ_NEXT(nl, entry);
	if (nl->nb->info.type == NB_PIECE
	    && (nl != BTPDQ_FIRST(&p->outq) && p->outq_off > 0)) {
	    nb_drop(nl->nb);
	    BTPDQ_REMOVE(&p->outq, nl, entry);
	    free(nl);
	    nl = next;
	    next = BTPDQ_NEXT(next, entry);
	    nb_drop(nl->nb);
	    BTPDQ_REMOVE(&p->outq, nl, entry);
	    free(nl);
	}
	nl = next;
    }

    p->flags |= PF_I_CHOKE;
    net_send_choke(p);    
}

void
peer_want(struct peer *p, uint32_t index)
{
    assert(p->nwant < p->npieces);
    p->nwant++;
    if (p->nwant == 1) {
	p->flags |= PF_I_WANT;
	net_send_interest(p);
    }
}

void
peer_unwant(struct peer *p, uint32_t index)
{
    assert(p->nwant > 0);
    p->nwant--;
    if (p->nwant == 0) {
	p->flags &= ~PF_I_WANT;
	net_send_uninterest(p);
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
    net_handshake(p, 1);
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
    net_handshake(p, 0);
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
    net_handshake(p, 0);
}

void
peer_on_choke(struct peer *p)
{
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
    if (!has_bit(p->piece_field, index)) {
	set_bit(p->piece_field, index);
	p->npieces++;
	cm_on_piece_ann(p, index);
    }
}

void
peer_on_bitfield(struct peer *p, uint8_t *field)
{
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
    struct piece_req *req = BTPDQ_FIRST(&p->my_reqs);
    if (req != NULL &&
	req->index == index &&
	req->begin == begin &&
	req->length == length) {

	assert(p->nreqs_out > 0);
	p->nreqs_out--;
	BTPDQ_REMOVE(&p->my_reqs, req, entry);
	free(req);
	
	cm_on_block(p, index, begin, length, data);
    }
}

void
peer_on_request(struct peer *p, uint32_t index, uint32_t begin,
    uint32_t length)
{
    off_t cbegin = index * p->tp->meta.piece_length + begin;
    char * content = torrent_get_bytes(p->tp, cbegin, length);
    net_send_piece(p, index, begin, content, length);
}

void
peer_on_cancel(struct peer *p, uint32_t index, uint32_t begin,
    uint32_t length)
{
    struct nb_link *nl = BTPDQ_FIRST(&p->outq);
    while (nl != NULL) {
	if (nl->nb->info.type == NB_PIECE
	    && nl->nb->info.index == index
	    && nl->nb->info.begin == begin
	    && nl->nb->info.length == length
	    && (nl != BTPDQ_FIRST(&p->outq) && p->outq_off > 0)) {
	    btpd_log(BTPD_L_MSG, "cancel matched.\n");
	    struct nb_link *data = BTPDQ_NEXT(nl, entry);
	    nb_drop(nl->nb);
	    BTPDQ_REMOVE(&p->outq, nl, entry);
	    free(nl);
	    nb_drop(data->nb);
	    BTPDQ_REMOVE(&p->outq, data, entry);
	    free(data);
	}
	nl = BTPDQ_NEXT(nl, entry);
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
