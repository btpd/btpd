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

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

#define min(x, y) ((x) <= (y) ? (x) : (y))

void
net_write32(void *buf, uint32_t num)
{
    *(uint32_t *)buf = htonl(num);
}

uint32_t
net_read32(const void *buf)
{
    return ntohl(*(uint32_t *)buf);
}

static unsigned long
net_write(struct peer *p, unsigned long wmax)
{
    struct nb_link *nl;
    struct iovec iov[IOV_MAX];
    int niov;
    int limited;
    ssize_t nwritten;
    unsigned long bcount;

    limited = wmax > 0;

    niov = 0;
    assert((nl = BTPDQ_FIRST(&p->outq)) != NULL);
    while ((niov < IOV_MAX && nl != NULL
	       && (!limited || (limited && wmax > 0)))) {
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
	btpd_log(BTPD_L_CONN, "connection closed by peer.\n");
	peer_kill(p);
	return 0;
    }

    bcount = nwritten;

    nl = BTPDQ_FIRST(&p->outq);
    while (bcount > 0) {
	unsigned long bufdelta = nl->nb->len - p->outq_off;
	if (bcount >= bufdelta) {
	    peer_sent(p, nl->nb);
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

    return nwritten;
}

void
net_set_state(struct peer *p, int state, size_t size)
{
    p->net_state = state;
    p->state_bytes = size;
}

static int
net_dispatch_msg(struct peer *p, const char *buf)
{
    uint32_t index, begin, length;
    int res = 0;

    switch (p->msg_num) {
    case MSG_CHOKE:
	peer_on_choke(p);
	break;
    case MSG_UNCHOKE:
	peer_on_unchoke(p);
	break;
    case MSG_INTEREST:
	peer_on_interest(p);
	break;
    case MSG_UNINTEREST:
	peer_on_uninterest(p);
	break;
    case MSG_HAVE:
	peer_on_have(p, net_read32(buf));
	break;
    case MSG_BITFIELD:
	peer_on_bitfield(p, buf);
	break;
    case MSG_REQUEST:
	if ((p->flags & (PF_P_WANT|PF_I_CHOKE)) == PF_P_WANT) {
	    index = net_read32(buf);
	    begin = net_read32(buf + 4);
	    length = net_read32(buf + 8);
	    if ((length > PIECE_BLOCKLEN
		    || index >= p->tp->meta.npieces
		    || !has_bit(p->tp->piece_field, index)
		    || begin + length < torrent_piece_size(p->tp, index))) {
		res = 1;
		break;
	    }
	    peer_on_request(p, index, begin, length);
	}
	break;
    case MSG_CANCEL:
	index = net_read32(buf);
	begin = net_read32(buf + 4);
	length = net_read32(buf + 8);
	peer_on_cancel(p, index, begin, length);
	break;
    case MSG_PIECE:
	index = net_read32(buf);
	begin = net_read32(buf + 4);
	length = p->msg_len - 9;
	peer_on_piece(p, index, begin, length, buf + 8);
	break;
    default:
	abort();
    }
    return res;
}

static int
net_mh_ok(struct peer *p)
{
    uint32_t mlen = p->msg_len;
    switch (p->msg_num) {
    case MSG_CHOKE:
    case MSG_UNCHOKE:
    case MSG_INTEREST:
    case MSG_UNINTEREST:
	return mlen == 1;
    case MSG_HAVE:
	return mlen == 5;
    case MSG_BITFIELD:
	return mlen == (uint32_t)ceil(p->tp->meta.npieces / 8.0) + 1;
    case MSG_REQUEST:
    case MSG_CANCEL:
	return mlen == 13;
    case MSG_PIECE:
	return mlen <= PIECE_BLOCKLEN + 9;
    default:
	return 0;
    }
}

static void
net_progress(struct peer *p, size_t length)
{
    if (p->net_state == NET_MSGBODY && p->msg_num == MSG_PIECE) {
	p->tp->downloaded += length;
	p->rate_to_me[btpd.seconds % RATEHISTORY] += length;
    }
}

static int
net_state(struct peer *p, const char *buf)
{
    switch (p->net_state) {
    case SHAKE_PSTR:
        if (bcmp(buf, "\x13""BitTorrent protocol", 20) != 0)
	    goto bad;
	net_set_state(p, SHAKE_INFO, 20);
        break;
    case SHAKE_INFO:
	if (p->flags & PF_INCOMING) {
	    struct torrent *tp = torrent_get_by_hash(buf);
	    if (tp == NULL)
		goto bad;
	    p->tp = tp;
	    peer_send(p, nb_create_shake(p->tp));
	} else if (bcmp(buf, p->tp->meta.info_hash, 20) != 0)
	    goto bad;
	net_set_state(p, SHAKE_ID, 20);
        break;
    case SHAKE_ID:
	if ((torrent_has_peer(p->tp, buf)
             || bcmp(buf, btpd.peer_id, 20) == 0))
	    goto bad;
	bcopy(buf, p->id, 20);
	btpd_log(BTPD_L_CONN, "Got whole shake.\n");
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
	net_set_state(p, NET_MSGSIZE, 4);
        break;
    case NET_MSGSIZE:
	p->msg_len = net_read32(buf);
	if (p->msg_len != 0)
	    net_set_state(p, NET_MSGHEAD, 1);
        break;
    case NET_MSGHEAD:
	p->msg_num = buf[0];
	if (!net_mh_ok(p))
	    goto bad;
	else if (p->msg_len == 1) {
	    if (net_dispatch_msg(p, buf) != 0)
		goto bad;
	    net_set_state(p, NET_MSGSIZE, 4);
	} else {
	    net_set_state(p, NET_MSGBODY, p->msg_len - 1);
	}
        break;
    case NET_MSGBODY:
	if (net_dispatch_msg(p, buf) != 0)
	    goto bad;
	net_set_state(p, NET_MSGSIZE, 4);
        break;
    default:
	abort();
    }

    return 0;
bad:
    btpd_log(BTPD_L_CONN, "bad data from %p.\n", p);
    peer_kill(p);
    return -1;
}

#define GRBUFLEN (1 << 15)

static unsigned long
net_read(struct peer *p, unsigned long rmax)
{
    size_t rest = p->net_in.buf_len - p->net_in.buf_off;
    char buf[GRBUFLEN];
    struct iovec iov[2] = {
	{
	    p->net_in.buf + p->net_in.buf_off,
	    rest
	}, {
	    buf,
	    sizeof(buf)
	}
    };

    if (rmax > 0) {
	if (iov[0].iov_len > rmax)
	    iov[0].iov_len = rmax;
	iov[1].iov_len = min(rmax - iov[0].iov_len, iov[1].iov_len);
    }

    ssize_t nread = readv(p->sd, iov, 2);
    if (nread < 0 && errno == EAGAIN)
	goto out;
    else if (nread < 0) {
	btpd_log(BTPD_L_CONN, "Read error (%s) on %p.\n", strerror(errno), p);
	peer_kill(p);
	return 0;
    } else if (nread == 0) {
	btpd_log(BTPD_L_CONN, "Connection closed by %p.\n", p);
	peer_kill(p);
	return 0;
    }

    if (rest > 0) {
	if (nread < rest) {
	    p->net_in.buf_off += nread;
	    net_progress(p, nread);
	    goto out;
	}
	net_progress(p, rest);
	if (net_state(p, p->net_in.buf) != 0)
	    return nread;
	free(p->net_in.buf);
	bzero(&p->net_in, sizeof(p->net_in));
    }

    struct io_buffer iob = { 0, nread - rest, buf };

    while (p->state_bytes <= iob.buf_len) {
        ssize_t consumed = p->state_bytes;
	net_progress(p, consumed);
	if (net_state(p, iob.buf) != 0)
	    return nread;
	iob.buf += consumed;
	iob.buf_len -= consumed;
    }

    if (iob.buf_len > 0) {
	net_progress(p, iob.buf_len);
	p->net_in.buf_off = iob.buf_len;
	p->net_in.buf_len = p->state_bytes;
	p->net_in.buf = btpd_malloc(p->state_bytes);
	bcopy(iob.buf, p->net_in.buf, iob.buf_len);
    }

out:
    event_add(&p->in_ev, NULL);
    return nread > 0 ? nread : 0;
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
	    btpd.ibw_left -= net_read(p, btpd.ibw_left);
	}
    } else {
	while ((p = BTPDQ_FIRST(&btpd.readq)) != NULL) {
	    BTPDQ_REMOVE(&btpd.readq, p, rq_entry);
	    p->flags &= ~PF_ON_READQ;
	    net_read(p, 0);
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

void
net_read_cb(int sd, short type, void *arg)
{
    struct peer *p = (struct peer *)arg;
    if (btpd.ibwlim == 0)
	net_read(p, 0);
    else if (btpd.ibw_left > 0)
	btpd.ibw_left -= net_read(p, btpd.ibw_left);
    else {
	p->flags |= PF_ON_READQ;
	BTPDQ_INSERT_TAIL(&btpd.readq, p, rq_entry);
    }
}

void
net_write_cb(int sd, short type, void *arg)
{
    struct peer *p = (struct peer *)arg;
    if (type == EV_TIMEOUT) {
	btpd_log(BTPD_L_CONN, "Write attempt timed out.\n");
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
