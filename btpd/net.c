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

static struct event m_bw_timer;
static unsigned long m_bw_bytes_in;
static unsigned long m_bw_bytes_out;

static unsigned long m_rate_up;
static unsigned long m_rate_dwn;

static struct event m_net_incoming;

static unsigned m_ntorrents;
static struct torrent_tq m_torrents = BTPDQ_HEAD_INITIALIZER(m_torrents);

unsigned net_npeers;

struct peer_tq net_bw_readq = BTPDQ_HEAD_INITIALIZER(net_bw_readq);
struct peer_tq net_bw_writeq = BTPDQ_HEAD_INITIALIZER(net_bw_writeq);
struct peer_tq net_unattached = BTPDQ_HEAD_INITIALIZER(net_unattached);

void
net_add_torrent(struct torrent *tp)
{
    tp->net_active = 1;
    BTPDQ_INSERT_HEAD(&m_torrents, tp, net_entry);
    m_ntorrents++;
    dl_start(tp);
}

void
net_del_torrent(struct torrent *tp)
{
    tp->net_active = 0;
    assert(m_ntorrents > 0);
    m_ntorrents--;
    BTPDQ_REMOVE(&m_torrents, tp, net_entry);

    ul_on_lost_torrent(tp);
    dl_stop(tp);

    struct peer *p = BTPDQ_FIRST(&net_unattached);
    while (p != NULL) {
        struct peer *next = BTPDQ_NEXT(p, p_entry);
        if (p->tp == tp)
            peer_kill(p);
        p = next;
    }

    p = BTPDQ_FIRST(&tp->peers);
    while (p != NULL) {
        struct peer *next = BTPDQ_NEXT(p, p_entry);
        peer_kill(p);
        p = next;
    }
}

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
                p->count_up += bufdelta;
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
                p->count_up += bcount;
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
net_set_state(struct peer *p, enum net_state state, size_t size)
{
    p->net.state = state;
    p->net.st_bytes = size;
}

static int
net_dispatch_msg(struct peer *p, const char *buf)
{
    uint32_t index, begin, length;
    int res = 0;

    switch (p->net.msg_num) {
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
        if (p->npieces == 0)
            peer_on_bitfield(p, buf);
        else
            res = 1;
        break;
    case MSG_REQUEST:
        if ((p->flags & (PF_P_WANT|PF_I_CHOKE)) == PF_P_WANT) {
            index = net_read32(buf);
            begin = net_read32(buf + 4);
            length = net_read32(buf + 8);
            if ((length > PIECE_BLOCKLEN
                    || index >= p->tp->meta.npieces
                    || !has_bit(p->tp->piece_field, index)
                    || begin + length > torrent_piece_size(p->tp, index))) {
                btpd_log(BTPD_L_MSG, "bad request: (%u, %u, %u) from %p\n",
                         index, begin, length, p);
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
        length = p->net.msg_len - 9;
        peer_on_piece(p, p->net.pc_index, p->net.pc_begin, length, buf);
        break;
    default:
        abort();
    }
    return res;
}

static int
net_mh_ok(struct peer *p)
{
    uint32_t mlen = p->net.msg_len;
    switch (p->net.msg_num) {
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
    if (p->net.state == BTP_MSGBODY && p->net.msg_num == MSG_PIECE) {
        p->tp->downloaded += length;
        p->count_dwn += length;
    }
}

static int
net_state(struct peer *p, const char *buf)
{
    switch (p->net.state) {
    case SHAKE_PSTR:
        if (bcmp(buf, "\x13""BitTorrent protocol", 20) != 0)
            goto bad;
        net_set_state(p, SHAKE_INFO, 20);
        break;
    case SHAKE_INFO:
        if (p->flags & PF_INCOMING) {
            struct torrent *tp;
            BTPDQ_FOREACH(tp, &m_torrents, net_entry)
                if (bcmp(buf, tp->meta.info_hash, 20) == 0)
                    break;
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
             || bcmp(buf, btpd_get_peer_id(), 20) == 0))
            goto bad;
        bcopy(buf, p->id, 20);
        peer_on_shake(p);
        net_set_state(p, BTP_MSGSIZE, 4);
        break;
    case BTP_MSGSIZE:
        p->net.msg_len = net_read32(buf);
        if (p->net.msg_len == 0)
            peer_on_keepalive(p);
        else
            net_set_state(p, BTP_MSGHEAD, 1);
        break;
    case BTP_MSGHEAD:
        p->net.msg_num = buf[0];
        if (!net_mh_ok(p))
            goto bad;
        else if (p->net.msg_len == 1) {
            if (net_dispatch_msg(p, buf) != 0)
                goto bad;
            net_set_state(p, BTP_MSGSIZE, 4);
        } else if (p->net.msg_num == MSG_PIECE)
            net_set_state(p, BTP_PIECEMETA, 8);
        else
            net_set_state(p, BTP_MSGBODY, p->net.msg_len - 1);
        break;
    case BTP_PIECEMETA:
        p->net.pc_index = net_read32(buf);
        p->net.pc_begin = net_read32(buf + 4);
        net_set_state(p, BTP_MSGBODY, p->net.msg_len - 9);
        break;
    case BTP_MSGBODY:
        if (net_dispatch_msg(p, buf) != 0)
            goto bad;
        net_set_state(p, BTP_MSGSIZE, 4);
        break;
    default:
        abort();
    }

    return 0;
bad:
    btpd_log(BTPD_L_CONN, "bad data from %p (%u, %u, %u).\n",
             p, p->net.state, p->net.msg_len, p->net.msg_num);
    peer_kill(p);
    return -1;
}

#define GRBUFLEN (1 << 15)

static unsigned long
net_read(struct peer *p, unsigned long rmax)
{
    size_t rest = p->net.buf != NULL ? p->net.st_bytes - p->net.off : 0;
    char buf[GRBUFLEN];
    struct iovec iov[2] = {
        {
            p->net.buf + p->net.off,
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
            p->net.off += nread;
            net_progress(p, nread);
            goto out;
        }
        net_progress(p, rest);
        if (net_state(p, p->net.buf) != 0)
            return nread;
        free(p->net.buf);
        p->net.buf = NULL;
        p->net.off = 0;
    }

    iov[1].iov_len = nread - rest;
    while (p->net.st_bytes <= iov[1].iov_len) {
        size_t consumed = p->net.st_bytes;
        net_progress(p, consumed);
        if (net_state(p, iov[1].iov_base) != 0)
            return nread;
        iov[1].iov_base += consumed;
        iov[1].iov_len -= consumed;
    }

    if (iov[1].iov_len > 0) {
        net_progress(p, iov[1].iov_len);
        p->net.off = iov[1].iov_len;
        p->net.buf = btpd_malloc(p->net.st_bytes);
        bcopy(iov[1].iov_base, p->net.buf, iov[1].iov_len);
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

    assert(net_npeers < net_max_peers);

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

    assert(net_npeers <= net_max_peers);
    if (net_npeers == net_max_peers) {
        close(nsd);
        return;
    }

    peer_create_in(nsd);

    btpd_log(BTPD_L_CONN, "got connection.\n");
}

#define RATEHISTORY 20

static unsigned long
compute_rate_sub(unsigned long rate)
{
    if (rate > 256 * RATEHISTORY)
        return rate / RATEHISTORY;
    else
        return min(256, rate);
}

static void
compute_rates(void) {
    unsigned long tot_up = 0, tot_dwn = 0;
    struct torrent *tp;
    BTPDQ_FOREACH(tp, &m_torrents, net_entry) {
        unsigned long tp_up = 0, tp_dwn = 0;
        struct peer *p;
        BTPDQ_FOREACH(p, &tp->peers, p_entry) {
            if (p->count_up > 0 || peer_active_up(p)) {
                tp_up += p->count_up;
                p->rate_up += p->count_up - compute_rate_sub(p->rate_up);
                p->count_up = 0;
            }
            if (p->count_dwn > 0 || peer_active_down(p)) {
                tp_dwn += p->count_dwn;
                p->rate_dwn += p->count_dwn - compute_rate_sub(p->rate_dwn);
                p->count_dwn = 0;
            }
        }
        tp->rate_up += tp_up - compute_rate_sub(tp->rate_up);
        tp->rate_dwn += tp_dwn - compute_rate_sub(tp->rate_dwn);
        tot_up += tp_up;
        tot_dwn += tp_dwn;
    }
    m_rate_up += tot_up - compute_rate_sub(m_rate_up);
    m_rate_dwn += tot_dwn - compute_rate_sub(m_rate_dwn);
    btpd_log(BTPD_L_BTPD, "rates: %7.2fkB/s, %7.2fkB/s.\n",
        (double)m_rate_up / 20 / (1 << 10),
        (double)m_rate_dwn / 20 / (1 << 10));
}

void
net_bw_cb(int sd, short type, void *arg)
{
    struct peer *p;

    evtimer_add(&m_bw_timer, (& (struct timeval) { 1, 0 }));

    compute_rates();

    m_bw_bytes_out = net_bw_limit_out;
    m_bw_bytes_in = net_bw_limit_in;

    if (net_bw_limit_in > 0) {
        while ((p = BTPDQ_FIRST(&net_bw_readq)) != NULL && m_bw_bytes_in > 0) {
            BTPDQ_REMOVE(&net_bw_readq, p, rq_entry);
            p->flags &= ~PF_ON_READQ;
            m_bw_bytes_in -= net_read(p, m_bw_bytes_in);
        }
    } else {
        while ((p = BTPDQ_FIRST(&net_bw_readq)) != NULL) {
            BTPDQ_REMOVE(&net_bw_readq, p, rq_entry);
            p->flags &= ~PF_ON_READQ;
            net_read(p, 0);
        }
    }

    if (net_bw_limit_out) {
        while ((p = BTPDQ_FIRST(&net_bw_writeq)) != NULL && m_bw_bytes_out > 0) {
            BTPDQ_REMOVE(&net_bw_writeq, p, wq_entry);
            p->flags &= ~PF_ON_WRITEQ;
            m_bw_bytes_out -=  net_write(p, m_bw_bytes_out);
        }
    } else {
        while ((p = BTPDQ_FIRST(&net_bw_writeq)) != NULL) {
            BTPDQ_REMOVE(&net_bw_writeq, p, wq_entry);
            p->flags &= ~PF_ON_WRITEQ;
            net_write(p, 0);
        }
    }
}

void
net_read_cb(int sd, short type, void *arg)
{
    struct peer *p = (struct peer *)arg;
    if (net_bw_limit_in == 0)
        net_read(p, 0);
    else if (m_bw_bytes_in > 0)
        m_bw_bytes_in -= net_read(p, m_bw_bytes_in);
    else {
        p->flags |= PF_ON_READQ;
        BTPDQ_INSERT_TAIL(&net_bw_readq, p, rq_entry);
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
    if (net_bw_limit_out == 0) {
        net_write(p, 0);
    } else if (m_bw_bytes_out > 0) {
        m_bw_bytes_out -= net_write(p, m_bw_bytes_out);
    } else {
        p->flags |= PF_ON_WRITEQ;
        BTPDQ_INSERT_TAIL(&net_bw_writeq, p, wq_entry);
    }
}

void
net_init(void)
{
    m_bw_bytes_out = net_bw_limit_out;
    m_bw_bytes_in = net_bw_limit_in;

    int nfiles = getdtablesize();
    if (nfiles <= 20)
        btpd_err("Too few open files allowed (%d). "
                 "Check \"ulimit -n\"\n", nfiles);
    else if (nfiles < 64)
        btpd_log(BTPD_L_BTPD,
                 "You have restricted the number of open files to %d. "
                 "More could be beneficial to the download performance.\n",
                 nfiles);
    net_max_peers = nfiles - 20;

    int sd;
    int flag = 1;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(net_port);

    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        btpd_err("socket: %s\n", strerror(errno));
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
        btpd_err("bind: %s\n", strerror(errno));
    listen(sd, 10);
    set_nonblocking(sd);

    event_set(&m_net_incoming, sd, EV_READ | EV_PERSIST,
        net_connection_cb, NULL);
    event_add(&m_net_incoming, NULL);

    evtimer_set(&m_bw_timer, net_bw_cb, NULL);
    evtimer_add(&m_bw_timer, (& (struct timeval) { 1, 0 }));
}
