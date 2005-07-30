#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <sys/mman.h>
#include <sys/wait.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btpd.h"

#define WRITE_TIMEOUT (& (struct timeval) { 60, 0 })

#define min(x, y) ((x) <= (y) ? (x) : (y))

static unsigned long
net_write(struct peer *p, unsigned long wmax);

void
net_read_cb(int sd, short type, void *arg)
{
    struct peer *p = (struct peer *)arg;
    if (btpd.ibwlim == 0) {
	p->reader->read(p, 0);
    } else if (btpd.ibw_left > 0) {
	btpd.ibw_left -= p->reader->read(p, btpd.ibw_left);
    } else {
	p->flags |= PF_ON_READQ;
	BTPDQ_INSERT_TAIL(&btpd.readq, p, rq_entry);
    }
}

void
net_write_cb(int sd, short type, void *arg)
{
    struct peer *p = (struct peer *)arg;
    if (type == EV_TIMEOUT) {
	btpd_log(BTPD_L_ERROR, "Write attempt timed out.\n");
	peer_kill(p);
	return;
    }
    if (btpd.obwlim == 0) {
	net_write(p, 0);
    } else if (btpd.obw_left > 0) {
	btpd.obw_left -= net_write(p, btpd.obw_left);
    } else {
	p->flags |= PF_ON_WRITEQ;
	BTPDQ_INSERT_TAIL(&btpd.writeq, p, wq_entry);
    }
}

void
net_write32(void *buf, uint32_t num)
{
    *(uint32_t *)buf = htonl(num);
}

uint32_t
net_read32(void *buf)
{
    return ntohl(*(uint32_t *)buf);
}

static void
kill_buf_no(char *buf, size_t len)
{
    //Nothing
}

static void
kill_buf_free(char *buf, size_t len)
{
    free(buf);
}

int
nb_drop(struct net_buf *nb)
{
    assert(nb->refs > 0);
    nb->refs--;
    if (nb->refs == 0) {
	nb->kill_buf(nb->buf, nb->len);
	free(nb);
	return 1;
    } else
	return 0;
}

void
nb_hold(struct net_buf *nb)
{
    nb->refs++;
}

struct net_buf *
nb_create_alloc(short type, size_t len)
{
    struct net_buf *nb = btpd_calloc(1, sizeof(*nb) + len);
    nb->type = type;
    nb->buf = (char *)(nb + 1);
    nb->len = len;
    nb->kill_buf = kill_buf_no;
    return nb;
}

struct net_buf *
nb_create_set(short type, char *buf, size_t len,
    void (*kill_buf)(char *, size_t))
{
    struct net_buf *nb = btpd_calloc(1, sizeof(*nb));
    nb->type = type;
    nb->buf = buf;
    nb->len = len;
    nb->kill_buf = kill_buf;
    return nb;
}

uint32_t
nb_get_index(struct net_buf *nb)
{
    switch (nb->type) {
    case NB_CANCEL:
    case NB_HAVE:
    case NB_PIECE:
    case NB_REQUEST:
	return net_read32(nb->buf + 5);
    default:
	abort();
    }
}

uint32_t
nb_get_begin(struct net_buf *nb)
{
    switch (nb->type) {
    case NB_CANCEL:
    case NB_PIECE:
    case NB_REQUEST:
	return net_read32(nb->buf + 9);
    default:
	abort();
    }
}

uint32_t
nb_get_length(struct net_buf *nb)
{
    switch (nb->type) {
    case NB_CANCEL:
    case NB_REQUEST:
	return net_read32(nb->buf + 13);
    case NB_PIECE:
	return net_read32(nb->buf) - 9;
    default:
	abort();
    }
}

void
kill_shake(struct input_reader *reader)
{
    free(reader);
}

#define NIOV 16

static unsigned long
net_write(struct peer *p, unsigned long wmax)
{
    struct nb_link *nl;
    struct iovec iov[NIOV];
    int niov;
    int limited;
    ssize_t nwritten;
    unsigned long bcount;

    limited = wmax > 0;

    niov = 0;
    assert((nl = BTPDQ_FIRST(&p->outq)) != NULL);
    while (niov < NIOV && nl != NULL && (!limited || (limited && wmax > 0))) {
	if (niov > 0) {
	    iov[niov].iov_base = nl->nb->buf;
	    iov[niov].iov_len = nl->nb->len;
	} else {
	    iov[niov].iov_base = nl->nb->buf + p->outq_off;
	    iov[niov].iov_len = nl->nb->len - p->outq_off;
	}
	if (limited) {
	    if (iov[niov].iov_len > wmax)
		iov[niov].iov_len = wmax;
	    wmax -= iov[niov].iov_len;
	}
	niov++;
	nl = BTPDQ_NEXT(nl, entry);
    }

    nwritten = writev(p->sd, iov, niov);
    if (nwritten < 0) {
	if (errno == EAGAIN) {
	    event_add(&p->out_ev, WRITE_TIMEOUT);
	    return 0;
	} else {
	    btpd_log(BTPD_L_CONN, "write error: %s\n", strerror(errno));
	    peer_kill(p);
	    return 0;
	}
    } else if (nwritten == 0) {
	btpd_log(BTPD_L_CONN, "connection close by peer.\n");
	peer_kill(p);
	return 0;
    }

    bcount = nwritten;

    nl = BTPDQ_FIRST(&p->outq);
    while (bcount > 0) {
	unsigned long bufdelta = nl->nb->len - p->outq_off;
	if (bcount >= bufdelta) {
	    if (nl->nb->type == NB_TORRENTDATA) {
		p->tp->uploaded += bufdelta;
		p->rate_from_me[btpd.seconds % RATEHISTORY] += bufdelta;
	    }
	    bcount -= bufdelta;
	    BTPDQ_REMOVE(&p->outq, nl, entry);
	    nb_drop(nl->nb);
	    free(nl);
	    p->outq_off = 0;
	    nl = BTPDQ_FIRST(&p->outq);
	} else {
	    if (nl->nb->type == NB_TORRENTDATA) {
		p->tp->uploaded += bcount;
		p->rate_from_me[btpd.seconds % RATEHISTORY] += bcount;
	    }
	    p->outq_off +=  bcount;
	    bcount = 0;
	}
    }
    if (!BTPDQ_EMPTY(&p->outq))
	event_add(&p->out_ev, WRITE_TIMEOUT);
    else if (p->flags & PF_WRITE_CLOSE) {
	btpd_log(BTPD_L_CONN, "Closed because of write flag.\n");
	peer_kill(p);
    }

    return nwritten;
}

void
net_send(struct peer *p, struct net_buf *nb)
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
net_unsend(struct peer *p, struct nb_link *nl)
{
    if (!(nl == BTPDQ_FIRST(&p->outq) && p->outq_off > 0)) {
	BTPDQ_REMOVE(&p->outq, nl, entry);
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
net_send_piece(struct peer *p, uint32_t index, uint32_t begin,
	       char *block, size_t blen)
{
    struct net_buf *head, *piece;

    btpd_log(BTPD_L_MSG, "send piece: %u, %u, %u\n", index, begin, blen);

    head = nb_create_alloc(NB_PIECE, 13);
    net_write32(head->buf, 9 + blen);
    head->buf[4] = MSG_PIECE;
    net_write32(head->buf + 5, index);
    net_write32(head->buf + 9, begin);
    net_send(p, head);

    piece = nb_create_set(NB_TORRENTDATA, block, blen, kill_buf_free);
    net_send(p, piece);
}

void
net_send_request(struct peer *p, struct piece_req *req)
{
    struct net_buf *out = nb_create_alloc(NB_REQUEST, 17);
    net_write32(out->buf, 13);
    out->buf[4] = MSG_REQUEST;
    net_write32(out->buf + 5, req->index);
    net_write32(out->buf + 9, req->begin);
    net_write32(out->buf + 13, req->length);
    net_send(p, out);
}

void
net_send_cancel(struct peer *p, struct piece_req *req)
{
    struct net_buf *out = nb_create_alloc(NB_CANCEL, 17);
    net_write32(out->buf, 13);
    out->buf[4] = MSG_CANCEL;
    net_write32(out->buf + 5, req->index);
    net_write32(out->buf + 9, req->begin);
    net_write32(out->buf + 13, req->length);
    net_send(p, out);
}

void
net_send_have(struct peer *p, uint32_t index)
{
    struct net_buf *out = nb_create_alloc(NB_HAVE, 9);
    net_write32(out->buf, 5);
    out->buf[4] = MSG_HAVE;
    net_write32(out->buf + 5, index);
    net_send(p, out);
}

void
net_send_multihave(struct peer *p)
{
    struct torrent *tp = p->tp;
    struct net_buf *out = nb_create_alloc(NB_MULTIHAVE, 9 * tp->have_npieces);
    for (uint32_t i = 0, count = 0; count < tp->have_npieces; i++) {
	if (has_bit(tp->piece_field, i)) {
	    net_write32(out->buf + count * 9, 5);
	    out->buf[count * 9 + 4] = MSG_HAVE;
	    net_write32(out->buf + count * 9 + 5, i);
	    count++;
	}
    }
    net_send(p, out);
}

void
net_send_onesized(struct peer *p, char mtype, int btype)
{
    struct net_buf *out = nb_create_alloc(btype, 5);
    net_write32(out->buf, 1);
    out->buf[4] = mtype;
    net_send(p, out);    
}

void
net_send_unchoke(struct peer *p)
{
    net_send_onesized(p, MSG_UNCHOKE, NB_UNCHOKE);
}

void
net_send_choke(struct peer *p)
{
    net_send_onesized(p, MSG_CHOKE, NB_CHOKE);
}

void
net_send_uninterest(struct peer *p)
{
    net_send_onesized(p, MSG_UNINTEREST, NB_UNINTEREST);
}

void
net_send_interest(struct peer *p)
{
    net_send_onesized(p, MSG_INTEREST, NB_INTEREST);
}

void
net_send_bitfield(struct peer *p)
{
    uint32_t plen = ceil(p->tp->meta.npieces / 8.0);

    struct net_buf *out = nb_create_alloc(NB_BITFIELD, 5);
    net_write32(out->buf, plen + 1);
    out->buf[4] = MSG_BITFIELD;
    net_send(p, out);
    
    out = nb_create_set(NB_BITDATA, p->tp->piece_field, plen, kill_buf_no);
    net_send(p, out);
}

void
net_send_shake(struct peer *p)
{
    struct net_buf *out = nb_create_alloc(NB_SHAKE, 68);
    bcopy("\x13""BitTorrent protocol\0\0\0\0\0\0\0\0", out->buf, 28);
    bcopy(p->tp->meta.info_hash, out->buf + 28, 20);
    bcopy(btpd.peer_id, out->buf + 48, 20);
    net_send(p, out);
}

static void
kill_generic(struct input_reader *reader)
{
    free(reader);
}

static size_t
net_read(struct peer *p, char *buf, size_t len)
{
    ssize_t nread = read(p->sd, buf, len);
    if (nread < 0) {
	if (errno == EAGAIN) {
	    event_add(&p->in_ev, NULL);
	    return 0;
	} else {
	    btpd_log(BTPD_L_CONN, "read error: %s\n", strerror(errno));
	    peer_kill(p);
	    return 0;
	}
    } else if (nread == 0) {
	btpd_log(BTPD_L_CONN, "conn closed by other side.\n");
	if (!BTPDQ_EMPTY(&p->outq))
	    p->flags |= PF_WRITE_CLOSE;
	else
	    peer_kill(p);
	return 0;
    } else 
	return nread;
}

static size_t
net_read_to_buf(struct peer *p, struct io_buffer *iob, unsigned long rmax)
{
    if (rmax == 0)
	rmax = iob->buf_len - iob->buf_off;
    else
	rmax = min(rmax, iob->buf_len - iob->buf_off);

    assert(rmax > 0);
    size_t nread = net_read(p, iob->buf + iob->buf_off, rmax);
    if (nread > 0)
	iob->buf_off += nread;
    return nread;
}

void
kill_bitfield(struct input_reader *rd)
{
    free(rd);
}

static void net_generic_reader(struct peer *p);

static unsigned long
read_bitfield(struct peer *p, unsigned long rmax)
{
    struct bitfield_reader *rd = (struct bitfield_reader *)p->reader;

    size_t nread = net_read_to_buf(p, &rd->iob, rmax);
    if (nread == 0)
	return 0;

    if (rd->iob.buf_off == rd->iob.buf_len) {
	peer_on_bitfield(p, rd->iob.buf);
	free(rd);
	net_generic_reader(p);
    } else
	event_add(&p->in_ev, NULL);

    return nread;
}

void
kill_piece(struct input_reader *rd)
{
    free(rd);
}

static unsigned long
read_piece(struct peer *p, unsigned long rmax)
{
    struct piece_reader *rd = (struct piece_reader *)p->reader;

    size_t nread = net_read_to_buf(p, &rd->iob, rmax);
    if (nread == 0)
	return 0;

    p->rate_to_me[btpd.seconds % RATEHISTORY] += nread;
    p->tp->downloaded += nread;
    if (rd->iob.buf_off == rd->iob.buf_len) {
	peer_on_piece(p, rd->index, rd->begin, rd->iob.buf_len, rd->iob.buf);
	free(rd);
	net_generic_reader(p);
    } else
	event_add(&p->in_ev, NULL);

    return nread;
}

#define GRBUFLEN (1 << 15)

static unsigned long
net_generic_read(struct peer *p, unsigned long rmax)
{
    char buf[GRBUFLEN];
    struct io_buffer iob = { 0, GRBUFLEN, buf };
    struct generic_reader *gr = (struct generic_reader *)p->reader;
    size_t nread;
    size_t off, len;
    int got_part;

    if (gr->iob.buf_off > 0) {
	iob.buf_off = gr->iob.buf_off;
	bcopy(gr->iob.buf, iob.buf, iob.buf_off);
	gr->iob.buf_off = 0;
    }
    
    if ((nread = net_read_to_buf(p, &iob, rmax)) == 0)
	return 0;

    len = iob.buf_off;
    off = 0;

    got_part = 0;
    while (!got_part && len - off >= 4) {
	size_t msg_len = net_read32(buf + off);

	if (msg_len == 0) {	/* Keep alive */
	    off += 4;
	    continue;
	}
	if (len - off < 5) {
	    got_part = 1;
	    break;
	}

	switch (buf[off + 4]) {
	case MSG_CHOKE:
	    btpd_log(BTPD_L_MSG, "choke.\n");
	    if (msg_len != 1)
		goto bad_data;
	    peer_on_choke(p);
	    break;
	case MSG_UNCHOKE:
	    btpd_log(BTPD_L_MSG, "unchoke.\n");
	    if (msg_len != 1)
		goto bad_data;
	    peer_on_unchoke(p);
	    break;
	case MSG_INTEREST:
	    btpd_log(BTPD_L_MSG, "interested.\n");
	    if (msg_len != 1)
		goto bad_data;
	    peer_on_interest(p);
	    break;
	case MSG_UNINTEREST:
	    btpd_log(BTPD_L_MSG, "uninterested.\n");
	    if (msg_len != 1)
		goto bad_data;
	    peer_on_uninterest(p);
	    break;
	case MSG_HAVE:
	    btpd_log(BTPD_L_MSG, "have.\n");
	    if (msg_len != 5)
		goto bad_data;
	    else if (len - off >= msg_len + 4) {
		uint32_t index = net_read32(buf + off + 5);
		peer_on_have(p, index);
	    } else
		got_part = 1;
	    break;
	case MSG_BITFIELD:
	    btpd_log(BTPD_L_MSG, "bitfield.\n");
	    if (msg_len != (size_t)ceil(p->tp->meta.npieces / 8.0) + 1)
		goto bad_data;
	    else if (p->npieces != 0)
		goto bad_data;
	    else if (len - off >= msg_len + 4)
		peer_on_bitfield(p, buf + off + 5);
	    else {
		struct bitfield_reader *rp;
		size_t mem = sizeof(*rp) + msg_len - 1;
		p->reader->kill(p->reader);
		rp = btpd_calloc(1, mem);
		rp->rd.kill = kill_bitfield;
		rp->rd.read = read_bitfield;
		rp->iob.buf = (char *)rp + sizeof(*rp);
		rp->iob.buf_len = msg_len - 1;
		rp->iob.buf_off = len - off - 5;
		bcopy(buf + off + 5, rp->iob.buf, rp->iob.buf_off);
		p->reader = (struct input_reader *)rp;
		event_add(&p->in_ev, NULL);
		return nread;
	    }
	    break;
	case MSG_REQUEST:
	    btpd_log(BTPD_L_MSG, "request.\n");
	    if (msg_len != 13)
		goto bad_data;
	    else if (len - off >= msg_len + 4) {
		if ((p->flags & (PF_P_WANT|PF_I_CHOKE)) != PF_P_WANT)
		    break;
		uint32_t index, begin, length;
		index = net_read32(buf + off + 5);
		begin = net_read32(buf + off + 9);
		length = net_read32(buf + off + 13);
		if (length > (1 << 15))
		    goto bad_data;
		if (index >= p->tp->meta.npieces)
		    goto bad_data;
		if (!has_bit(p->tp->piece_field, index))
		    goto bad_data;
		if (begin + length > torrent_piece_size(p->tp, index))
		    goto bad_data;
		peer_on_request(p, index, begin, length);
	    } else
		got_part = 1;
	    break;
	case MSG_PIECE:
	    btpd_log(BTPD_L_MSG, "piece.\n");
	    if (msg_len < 10)
		goto bad_data;
	    else if (len - off >= 13) {
		uint32_t index = net_read32(buf + off + 5);
		uint32_t begin = net_read32(buf + off + 9);
		uint32_t length = msg_len - 9;
		if (len - off >= msg_len + 4) {
		    p->tp->downloaded += length;
		    p->rate_to_me[btpd.seconds % RATEHISTORY] += length;
		    peer_on_piece(p, index, begin, length, buf + off + 13);
		} else {
		    struct piece_reader *rp;
		    size_t mem = sizeof(*rp) + length;
		    p->reader->kill(p->reader);
		    rp = btpd_calloc(1, mem);
		    rp->rd.kill = kill_piece;
		    rp->rd.read = read_piece;
		    rp->index = index;
		    rp->begin = begin;
		    rp->iob.buf = (char *)rp + sizeof(*rp);
		    rp->iob.buf_len = length;
		    rp->iob.buf_off = len - off - 13;
		    bcopy(buf + off + 13, rp->iob.buf, rp->iob.buf_off);
		    p->reader = (struct input_reader *)rp;
		    event_add(&p->in_ev, NULL);
		    p->tp->downloaded += rp->iob.buf_off;
		    p->rate_to_me[btpd.seconds % RATEHISTORY] +=
			rp->iob.buf_off;
		    return nread;
		}
	    } else
		got_part = 1;
	    break;
	case MSG_CANCEL:
	    if (msg_len != 13)
		goto bad_data;
	    else if (len - off >= msg_len + 4) {
		uint32_t index = net_read32(buf + off + 5);
		uint32_t begin = net_read32(buf + off + 9);
		uint32_t length = net_read32(buf + off + 13);
		if (index > p->tp->meta.npieces)
		    goto bad_data;
		if (begin + length > torrent_piece_size(p->tp, index))
		    goto bad_data;
		btpd_log(BTPD_L_MSG, "cancel: %u, %u, %u\n",
		    index, begin, length);
		peer_on_cancel(p, index, begin, length);
	    } else
		got_part = 1;
	    break;
	default:
	    goto bad_data;
	}
	if (!got_part)
	    off += 4 + msg_len;
    }
    if (off != len) {
	gr->iob.buf_off = len - off;
        assert(gr->iob.buf_off <= gr->iob.buf_len);
	bcopy(buf + off, gr->iob.buf, gr->iob.buf_off);
    }
    event_add(&p->in_ev, NULL);
    return nread;

bad_data:
    btpd_log(BTPD_L_MSG, "bad data\n");
    peer_kill(p);
    return nread;
}

static void
net_generic_reader(struct peer *p)
{
    struct generic_reader *gr;
    gr = btpd_calloc(1, sizeof(*gr));

    gr->rd.read = net_generic_read;
    gr->rd.kill = kill_generic;

    gr->iob.buf = gr->_io_buf;
    gr->iob.buf_len = MAX_INPUT_LEFT;
    gr->iob.buf_off = 0;

    p->reader = (struct input_reader *)gr;

    event_add(&p->in_ev, NULL);
}

static unsigned long
net_shake_read(struct peer *p, unsigned long rmax)
{
    struct handshake *hs = (struct handshake *)p->reader;
    struct io_buffer *in = &hs->in;

    size_t nread = net_read_to_buf(p, in, rmax);
    if (nread == 0)
	return 0;

    switch (hs->state) {
    case SHAKE_INIT:
	if (in->buf_off < 20)
	    break;
	else if (bcmp(in->buf, "\x13""BitTorrent protocol", 20) == 0)
	    hs->state = SHAKE_PSTR;
	else
	    goto bad_shake;
    case SHAKE_PSTR:
	if (in->buf_off < 28)
	    break;
	else
	    hs->state = SHAKE_RESERVED;
    case SHAKE_RESERVED:
	if (in->buf_off < 48)
	    break;
	else if (hs->incoming) {
	    struct torrent *tp = torrent_get_by_hash(in->buf + 28);
	    if (tp != NULL) {
		hs->state = SHAKE_INFO;
		p->tp = tp;
		net_send_shake(p);
	    } else
		goto bad_shake;
	} else {
	    if (bcmp(in->buf + 28, p->tp->meta.info_hash, 20) == 0)
		hs->state = SHAKE_INFO;
	    else
		goto bad_shake;
	}
    case SHAKE_INFO:
	if (in->buf_off < 68)
	    break;
	else {
	    if (torrent_has_peer(p->tp, in->buf + 48))
		goto bad_shake; // Not really, but we're already connected.
	    else if (bcmp(in->buf + 48, btpd.peer_id, 20) == 0)
		goto bad_shake; // Connection from myself.
	    bcopy(in->buf + 48, p->id, 20);
	    hs->state = SHAKE_ID;
	}
    default:
	assert(hs->state == SHAKE_ID);
    }
    if (hs->state == SHAKE_ID) {
	btpd_log(BTPD_L_CONN, "Got whole shake.\n");
	free(hs);
	p->piece_field = btpd_calloc(1, (int)ceil(p->tp->meta.npieces / 8.0));
	cm_on_new_peer(p);
	net_generic_reader(p);
	if (p->tp->have_npieces > 0) {
	    if (p->tp->have_npieces * 9 < 5 + ceil(p->tp->meta.npieces / 8.0))
		net_send_multihave(p);
	    else
		net_send_bitfield(p);
	}
    } else
	event_add(&p->in_ev, NULL);

    return nread;

bad_shake:
    btpd_log(BTPD_L_CONN, "Bad shake(%d)\n", hs->state);
    peer_kill(p);
    return nread;
}


void
net_handshake(struct peer *p, int incoming)
{
    struct handshake *hs;

    hs = calloc(1, sizeof(*hs));
    hs->incoming = incoming;
    hs->state = SHAKE_INIT;

    hs->in.buf_len = SHAKE_LEN;
    hs->in.buf_off = 0;
    hs->in.buf = hs->_io_buf;

    p->reader = (struct input_reader *)hs;
    hs->rd.read = net_shake_read;
    hs->rd.kill = kill_shake;

    if (!incoming)
	net_send_shake(p);
}

int
net_connect2(struct sockaddr *sa, socklen_t salen, int *sd)
{
    if ((*sd = socket(PF_INET, SOCK_STREAM, 0)) == -1) 
	return errno;
    
    set_nonblocking(*sd);

    if (connect(*sd, sa, salen) == -1 && errno != EINPROGRESS) {
	btpd_log(BTPD_L_CONN, "Botched connection %s.", strerror(errno));
	close(*sd);
	return errno;
    }

    return 0;
}

int
net_connect(const char *ip, int port, int *sd)
{
    struct addrinfo hints, *res;
    char portstr[6];
    
    assert(btpd.npeers < btpd.maxpeers);

    if (snprintf(portstr, sizeof(portstr), "%d", port) >= sizeof(portstr))
	return EINVAL;
    bzero(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ip, portstr, &hints, &res) != 0)
	return errno;

    int error = net_connect2(res->ai_addr, res->ai_addrlen, sd);
    freeaddrinfo(res);
    return error;
}

void
net_connection_cb(int sd, short type, void *arg)
{
    int nsd;

    nsd = accept(sd, NULL, NULL);
    if (nsd < 0) {
	if (errno == EWOULDBLOCK || errno == ECONNABORTED)
	    return;
	else
	    btpd_err("accept4: %s\n", strerror(errno));
    }

    if (set_nonblocking(nsd) != 0) {
	close(nsd);
	return;
    }

    assert(btpd.npeers <= btpd.maxpeers);
    if (btpd.npeers == btpd.maxpeers) {
	close(nsd);
	return;
    }

    peer_create_in(nsd);

    btpd_log(BTPD_L_CONN, "got connection.\n");
}

void
net_bw_rate(void)
{
    unsigned sum = 0;
    for (int i = 0; i < BWCALLHISTORY - 1; i++) {
	btpd.bwrate[i] = btpd.bwrate[i + 1];
	sum += btpd.bwrate[i];
    }
    btpd.bwrate[BWCALLHISTORY - 1] = btpd.bwcalls;
    sum += btpd.bwrate[BWCALLHISTORY - 1];
    btpd.bwcalls = 0;
    btpd.bw_hz_avg = sum / 5.0;
}

void
net_bw_cb(int sd, short type, void *arg)    
{
    struct peer *p;

    btpd.bwcalls++;

    double avg_hz;
    if (btpd.seconds < BWCALLHISTORY)
	avg_hz = btpd.bw_hz;
    else
	avg_hz = btpd.bw_hz_avg;

    btpd.obw_left = btpd.obwlim / avg_hz;
    btpd.ibw_left = btpd.ibwlim / avg_hz;

    if (btpd.ibwlim > 0) {
	while ((p = BTPDQ_FIRST(&btpd.readq)) != NULL && btpd.ibw_left > 0) {
	    BTPDQ_REMOVE(&btpd.readq, p, rq_entry);
	    p->flags &= ~PF_ON_READQ;
	    btpd.ibw_left -= p->reader->read(p, btpd.ibw_left);
	}
    } else {
	while ((p = BTPDQ_FIRST(&btpd.readq)) != NULL) {
	    BTPDQ_REMOVE(&btpd.readq, p, rq_entry);
	    p->flags &= ~PF_ON_READQ;
	    p->reader->read(p, 0);
	}
    }

    if (btpd.obwlim) {
	while ((p = BTPDQ_FIRST(&btpd.writeq)) != NULL && btpd.obw_left > 0) {
	    BTPDQ_REMOVE(&btpd.writeq, p, wq_entry);
	    p->flags &= ~PF_ON_WRITEQ;
	    btpd.obw_left -=  net_write(p, btpd.obw_left);
	}
    } else {
	while ((p = BTPDQ_FIRST(&btpd.writeq)) != NULL) {
	    BTPDQ_REMOVE(&btpd.writeq, p, wq_entry);
	    p->flags &= ~PF_ON_WRITEQ;
	    net_write(p, 0);
	}
    }
    event_add(&btpd.bwlim, (& (struct timeval) { 0, 1000000 / btpd.bw_hz }));
}
