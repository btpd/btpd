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

#define min(x, y) ((x) <= (y) ? (x) : (y))

static unsigned long
net_write(struct peer *p, unsigned long wmax);

void
net_bw_read_cb(int sd, short type, void *arg)
{
    struct peer *p;
    struct bwlim *bw = arg;

    btpd.ibw_left += bw->count;
    assert(btpd.ibw_left <= btpd.ibwlim);

    unsigned long count = 0;

    while ((p = BTPDQ_FIRST(&btpd.readq)) != NULL && btpd.ibw_left - count > 0) {
	BTPDQ_REMOVE(&btpd.readq, p, rq_entry);
	p->flags &= ~PF_ON_READQ;
	count += p->reader->read(p, btpd.ibw_left - count);
    }
    btpd.ibw_left -= count;

    BTPDQ_REMOVE(&btpd.bwq, bw, entry);
    if (count == 0)
	free(bw);
    else {
	bw->count = count;
	event_add(&bw->timer, (& (struct timeval) { 1, 0 }));
	BTPDQ_INSERT_TAIL(&btpd.bwq, bw, entry);
    }
}

void
net_read_cb(int sd, short type, void *arg)
{
    struct peer *p = (struct peer *)arg;
    if (btpd.ibwlim == 0) {
	p->reader->read(p, 0);
    } else if (btpd.ibw_left > 0) {
	unsigned long nread = p->reader->read(p, btpd.ibw_left);
	if (nread > 0) {
	    struct bwlim *bw = btpd_calloc(1, sizeof(*bw));
	    evtimer_set(&bw->timer, net_bw_read_cb, bw);
	    evtimer_add(&bw->timer, (& (struct timeval) { 1, 0 }));
	    bw->count = nread;
	    btpd.ibw_left -= nread;
	    BTPDQ_INSERT_TAIL(&btpd.bwq, bw, entry);
	}
    } else {
	p->flags |= PF_ON_READQ;
	BTPDQ_INSERT_TAIL(&btpd.readq, p, rq_entry);
    }
}

void
net_bw_write_cb(int sd, short type, void *arg)
{
    struct peer *p;
    struct bwlim *bw = arg;

    btpd.obw_left += bw->count;
    assert(btpd.obw_left <= btpd.obwlim);

    unsigned long count = 0;

    while ((p = BTPDQ_FIRST(&btpd.writeq)) != NULL && btpd.obw_left - count > 0) {
	BTPDQ_REMOVE(&btpd.writeq, p, wq_entry);
	p->flags &= ~PF_ON_WRITEQ;
	count += net_write(p, btpd.obw_left - count);
    }
    btpd.obw_left -= count;

    BTPDQ_REMOVE(&btpd.bwq, bw, entry);
    if (count == 0)
	free(bw);
    else {
	bw->count = count;
	event_add(&bw->timer, (& (struct timeval) { 1, 0 }));
	BTPDQ_INSERT_TAIL(&btpd.bwq, bw, entry);
    }
}

void
net_write_cb(int sd, short type, void *arg)
{
    struct peer *p = (struct peer *)arg;
    if (btpd.obwlim == 0) {
	net_write(p, 0);
    } else if (btpd.obw_left > 0) {
	unsigned long nw = net_write(p, btpd.obw_left); 
	if (nw > 0) {
	    struct bwlim *bw = btpd_calloc(1, sizeof(*bw));
	    evtimer_set(&bw->timer, net_bw_write_cb, bw);
	    evtimer_add(&bw->timer, (& (struct timeval) { 1, 0 }));
	    bw->count = nw;
	    btpd.obw_left -= nw;
	    BTPDQ_INSERT_TAIL(&btpd.bwq, bw, entry);
	}
    } else {
	p->flags |= PF_ON_WRITEQ;
	BTPDQ_INSERT_TAIL(&btpd.writeq, p, wq_entry);
    }
}

static void
nokill_iob(struct io_buffer *iob)
{
    //Nothing
}

static void
kill_free_buf(struct io_buffer *iob)
{
    free(iob->buf);
}

static struct iob_link *
malloc_liob(size_t len)
{
    struct iob_link *iol;
    iol = (struct iob_link *)btpd_calloc(1, sizeof(*iol) + len);
    iol->iob.buf = (char *)(iol + 1);
    iol->iob.buf_len = len;
    iol->iob.buf_off = 0;
    iol->kill_buf = nokill_iob;
    return iol;
}

static struct iob_link *
salloc_liob(char *buf, size_t len, void (*kill_buf)(struct io_buffer *))
{
    struct iob_link *iol;
    iol = (struct iob_link *)btpd_calloc(1, sizeof(*iol));
    iol->iob.buf = buf;
    iol->iob.buf_len = len;
    iol->iob.buf_off = 0;
    iol->kill_buf = kill_buf;
    return iol;
}

void
net_unsend_piece(struct peer *p, struct piece_req *req)
{
    struct iob_link *piece;

    BTPDQ_REMOVE(&p->p_reqs, req, entry);

    piece = BTPDQ_NEXT(req->head, entry);
    BTPDQ_REMOVE(&p->outq, piece, entry);
    piece->kill_buf(&piece->iob);
    free(piece);

    BTPDQ_REMOVE(&p->outq, req->head, entry);
    req->head->kill_buf(&req->head->iob);
    free(req->head);
    free(req);

    if (BTPDQ_EMPTY(&p->outq)) {
	if (p->flags & PF_ON_WRITEQ) {
	    BTPDQ_REMOVE(&btpd.writeq, p, wq_entry);
	    p->flags &= ~PF_ON_WRITEQ;
	} else
	    event_del(&p->out_ev);
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
    struct iob_link *iol;
    struct piece_req *req;
    struct iovec iov[NIOV];
    int niov;
    int limited;
    ssize_t nwritten;
    unsigned long bcount;

    limited = wmax > 0;

    niov = 0;
    assert((iol = BTPDQ_FIRST(&p->outq)) != NULL);
    while (niov < NIOV && iol != NULL
	   && (!limited || (limited && wmax > 0))) {
	iov[niov].iov_base = iol->iob.buf + iol->iob.buf_off;
	iov[niov].iov_len = iol->iob.buf_len - iol->iob.buf_off;
	if (limited) {
	    if (iov[niov].iov_len > wmax)
		iov[niov].iov_len = wmax;
	    wmax -= iov[niov].iov_len;
	}
	niov++;
	iol = BTPDQ_NEXT(iol, entry);
    }

    nwritten = writev(p->sd, iov, niov);
    if (nwritten < 0) {
	if (errno == EAGAIN) {
	    event_add(&p->out_ev, NULL);
	    return 0;
	} else {
	    btpd_log(BTPD_L_CONN, "write error: %s\n", strerror(errno));
	    peer_kill(p);
	    return 0;
	}
    }

    bcount = nwritten;

    req = BTPDQ_FIRST(&p->p_reqs);
    iol = BTPDQ_FIRST(&p->outq);
    while (bcount > 0) {
	unsigned long bufdelta = iol->iob.buf_len - iol->iob.buf_off;
	if (req != NULL && req->head == iol) {
	    struct piece_req *next = BTPDQ_NEXT(req, entry);
	    BTPDQ_REMOVE(&p->p_reqs, req, entry);
	    free(req);
	    req = next;
	}
	if (bcount >= bufdelta) {
	    if (iol->upload) {
		p->tp->uploaded += bufdelta;
		p->rate_from_me[btpd.seconds % RATEHISTORY] += bufdelta;
	    }
	    bcount -= bufdelta;
	    BTPDQ_REMOVE(&p->outq, iol, entry);
	    iol->kill_buf(&iol->iob);
	    free(iol);
	    iol = BTPDQ_FIRST(&p->outq);
	} else {
	    if (iol->upload) {
		p->tp->uploaded += bcount;
		p->rate_from_me[btpd.seconds % RATEHISTORY] += bcount;
	    }
	    iol->iob.buf_off += bcount;
	    bcount = 0;
	}
    }
    if (!BTPDQ_EMPTY(&p->outq))
	event_add(&p->out_ev, NULL);
    else if (p->flags & PF_WRITE_CLOSE) {
	btpd_log(BTPD_L_CONN, "Closed because of write flag.\n");
	peer_kill(p);
    }

    return nwritten;
}

void
net_send(struct peer *p, struct iob_link *iol)
{
    if (BTPDQ_EMPTY(&p->outq))
	event_add(&p->out_ev, NULL);
    BTPDQ_INSERT_TAIL(&p->outq, iol, entry);
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

void
net_send_piece(struct peer *p, uint32_t index, uint32_t begin,
	       char *block, size_t blen)
{
    struct iob_link *head, *piece;
    struct piece_req *req;

    btpd_log(BTPD_L_MSG, "send piece: %u, %u, %u\n", index, begin, blen);

    head = malloc_liob(13);
    net_write32(head->iob.buf, 9 + blen);
    head->iob.buf[4] = MSG_PIECE;
    net_write32(head->iob.buf + 5, index);
    net_write32(head->iob.buf + 9, begin);
    net_send(p, head);

    piece = salloc_liob(block, blen, kill_free_buf);
    piece->upload = 1;
    net_send(p, piece);

    req = btpd_malloc(sizeof(*req));
    req->index = index;
    req->begin = begin;
    req->length = blen;
    req->head = head;
    BTPDQ_INSERT_TAIL(&p->p_reqs, req, entry);
}

void
net_send_request(struct peer *p, struct piece_req *req)
{
    struct iob_link *out;
    out = malloc_liob(17);
    net_write32(out->iob.buf, 13);
    out->iob.buf[4] = MSG_REQUEST;
    net_write32(out->iob.buf + 5, req->index);
    net_write32(out->iob.buf + 9, req->begin);
    net_write32(out->iob.buf + 13, req->length);
    net_send(p, out);
}

void
net_send_cancel(struct peer *p, struct piece_req *req)
{
    struct iob_link *out;
    out = malloc_liob(17);
    net_write32(out->iob.buf, 13);
    out->iob.buf[4] = MSG_CANCEL;
    net_write32(out->iob.buf + 5, req->index);
    net_write32(out->iob.buf + 9, req->begin);
    net_write32(out->iob.buf + 13, req->length);
    net_send(p, out);
}

void
net_send_have(struct peer *p, uint32_t index)
{
    struct iob_link *out;
    out = malloc_liob(9);
    net_write32(out->iob.buf, 5);
    out->iob.buf[4] = MSG_HAVE;
    net_write32(out->iob.buf + 5, index);
    net_send(p, out);
}

void
net_send_onesized(struct peer *p, char type)
{
    struct iob_link *out;
    out = malloc_liob(5);
    net_write32(out->iob.buf, 1);
    out->iob.buf[4] = type;
    net_send(p, out);    
}

void
net_send_unchoke(struct peer *p)
{
    net_send_onesized(p, MSG_UNCHOKE);
}

void
net_send_choke(struct peer *p)
{
    net_send_onesized(p, MSG_CHOKE);
}

void
net_send_uninterest(struct peer *p)
{
    net_send_onesized(p, MSG_UNINTEREST);
}

void
net_send_interest(struct peer *p)
{
    net_send_onesized(p, MSG_INTEREST);
}

void
net_send_bitfield(struct peer *p)
{
    struct iob_link *out;
    uint32_t plen;

    plen = (uint32_t)ceil(p->tp->meta.npieces / 8.0);
    out = malloc_liob(5);
    net_write32(out->iob.buf, plen + 1);
    out->iob.buf[4] = MSG_BITFIELD;
    net_send(p, out);
    
    out = salloc_liob(p->tp->piece_field, plen, nokill_iob);
    net_send(p, out);
}

void
net_send_shake(struct peer *p)
{
    struct iob_link *out;
    out = malloc_liob(68);
    bcopy("\x13""BitTorrent protocol\0\0\0\0\0\0\0\0", out->iob.buf, 28);
    bcopy(p->tp->meta.info_hash, out->iob.buf + 28, 20);
    bcopy(btpd.peer_id, out->iob.buf + 48, 20);
    net_send(p, out);

    if (p->tp->have_npieces > 0)
	net_send_bitfield(p);
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
net_by_second(void)
{
    struct peer *p;
    struct torrent *tp;
    int ri = btpd.seconds % RATEHISTORY;

    BTPDQ_FOREACH(tp, &btpd.cm_list, entry) {
	BTPDQ_FOREACH(p, &tp->peers, cm_entry) {
	    p->rate_to_me[ri] = 0;
	    p->rate_from_me[ri] = 0;
	}
    }
}
