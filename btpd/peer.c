#include <sys/types.h>
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
    struct iob_link *iol;
    struct piece_req *req;

    btpd_log(BTPD_L_CONN, "killed peer.\n");

    if (p->flags & PF_ATTACHED)
	cm_on_lost_peer(p);
    if (p->flags & PF_ON_READQ)
	TAILQ_REMOVE(&btpd.readq, p, rq_entry);
    if (p->flags & PF_ON_WRITEQ)
	TAILQ_REMOVE(&btpd.writeq, p, wq_entry);

    close(p->sd);
    event_del(&p->in_ev);
    event_del(&p->out_ev);

    iol = TAILQ_FIRST(&p->outq);
    while (iol != NULL) {
	struct iob_link *next = TAILQ_NEXT(iol, entry);
	iol->kill_buf(&iol->iob);
	free(iol);
	iol = next;
    }
    req = TAILQ_FIRST(&p->p_reqs);
    while (req != NULL) {
	struct piece_req *next = TAILQ_NEXT(req, entry);
	free(req);
	req = next;
    }
    req = TAILQ_FIRST(&p->my_reqs);
    while (req != NULL) {
	struct piece_req *next = TAILQ_NEXT(req, entry);
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
    struct piece_req *req = btpd_calloc(1, sizeof(*req));
    req->index = index;
    req->begin = begin;
    req->length = len;
    TAILQ_INSERT_TAIL(&p->my_reqs, req, entry);
    net_send_request(p, req);
}

void
peer_cancel(struct peer *p, uint32_t index, uint32_t begin, uint32_t len)
{
    struct piece_req *req;
again:
    req = TAILQ_FIRST(&p->my_reqs);
    while (req != NULL &&
	   !(index == req->index &&
	     begin == req->begin &&
	     len == req->length))
	req = TAILQ_NEXT(req, entry);
    if (req != NULL) {
	net_send_cancel(p, req);
	TAILQ_REMOVE(&p->my_reqs, req, entry);
	free(req);
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
    struct piece_req *req;

    while ((req = TAILQ_FIRST(&p->p_reqs)) != NULL)
	net_unsend_piece(p, req);

    p->flags |= PF_I_CHOKE;
    net_send_choke(p);    
}

void
peer_want(struct peer *p, uint32_t index)
{
    p->nwant++;
    if (p->nwant == 1) {
	p->flags |= PF_I_WANT;
	net_send_interest(p);
    }
}

void
peer_unwant(struct peer *p, uint32_t index)
{
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
    TAILQ_INIT(&p->p_reqs);
    TAILQ_INIT(&p->my_reqs);
    TAILQ_INIT(&p->outq);

    event_set(&p->out_ev, p->sd, EV_WRITE, net_write_cb, p);
    event_set(&p->in_ev, p->sd, EV_READ, net_read_cb, p);
    event_add(&p->in_ev, NULL);

    return p;
}

void
peer_create_in(int sd)
{
    struct peer *p = peer_create_common(sd);
    net_handshake(p, 1);
}

void
peer_create_out(struct torrent *tp,
		const uint8_t *id,
		const char *ip,
		int port)
{
    int sd;
    struct peer *p;

    if (net_connect(ip, port, &sd) != 0)
	return;

    p = peer_create_common(sd);
    p->tp = tp;
    bcopy(id, p->id, 20);
    net_handshake(p, 0);
}
