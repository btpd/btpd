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

static unsigned long m_bw_bytes_in;
static unsigned long m_bw_bytes_out;

static unsigned long m_rate_up;
static unsigned long m_rate_dwn;

static struct event m_net_incoming;

unsigned net_npeers;

struct peer_tq net_bw_readq = BTPDQ_HEAD_INITIALIZER(net_bw_readq);
struct peer_tq net_bw_writeq = BTPDQ_HEAD_INITIALIZER(net_bw_writeq);
struct peer_tq net_unattached = BTPDQ_HEAD_INITIALIZER(net_unattached);

int
net_torrent_has_peer(struct net *n, const uint8_t *id)
{
    int has = 0;
    struct peer *p = BTPDQ_FIRST(&n->peers);
    while (p != NULL) {
        if (bcmp(p->id, id, 20) == 0) {
            has = 1;
            break;
        }
        p = BTPDQ_NEXT(p, p_entry);
    }
    return has;
}

void
net_create(struct torrent *tp)
{
    size_t field_size = ceil(tp->npieces / 8.0);
    size_t mem = sizeof(*(tp->net)) + field_size +
        tp->npieces * sizeof(*(tp->net->piece_count));

    struct net *n = btpd_calloc(1, mem);
    n->tp = tp;
    tp->net = n;

    BTPDQ_INIT(&n->getlst);

    n->busy_field = (uint8_t *)(n + 1);
    n->piece_count = (unsigned *)(n->busy_field + field_size);
}

void
net_kill(struct torrent *tp)
{
    free(tp->net);
    tp->net = NULL;
}

void
net_start(struct torrent *tp)
{
    struct net *n = tp->net;
    n->active = 1;
}

void
net_stop(struct torrent *tp)
{
    struct net *n = tp->net;

    n->active = 0;
    n->rate_up = 0;
    n->rate_dwn = 0;

    ul_on_lost_torrent(n);

    struct piece *pc;
    while ((pc = BTPDQ_FIRST(&n->getlst)) != NULL)
        piece_free(pc);
    BTPDQ_INIT(&n->getlst);

    struct peer *p = BTPDQ_FIRST(&net_unattached);
    while (p != NULL) {
        struct peer *next = BTPDQ_NEXT(p, p_entry);
        if (p->n == n)
            peer_kill(p);
        p = next;
    }

    p = BTPDQ_FIRST(&n->peers);
    while (p != NULL) {
        struct peer *next = BTPDQ_NEXT(p, p_entry);
        peer_kill(p);
        p = next;
    }
}

int
net_active(struct torrent *tp)
{
    return tp->net->active;
}

void
net_write32(void *buf, uint32_t num)
{
    uint8_t *p = buf;
    *p = (num >> 24) & 0xff;
    *(p + 1) = (num >> 16) & 0xff;
    *(p + 2) = (num >> 8) & 0xff;
    *(p + 3) = num & 0xff;
}

uint32_t
net_read32(const void *buf)
{
    const uint8_t *p = buf;
    return (uint32_t)*p << 24 | (uint32_t)*(p + 1) << 16
        | (uint16_t)*(p + 2) << 8 | *(p + 3);
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
            btpd_ev_add(&p->out_ev, NULL);
            p->t_wantwrite = btpd_seconds;
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
                p->n->uploaded += bufdelta;
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
                p->n->uploaded += bcount;
                p->count_up += bcount;
            }
            p->outq_off +=  bcount;
            bcount = 0;
        }
    }
    if (!BTPDQ_EMPTY(&p->outq)) {
        btpd_ev_add(&p->out_ev, NULL);
        p->t_wantwrite = btpd_seconds;
    }
    p->t_lastwrite = btpd_seconds;

    return nwritten;
}

static int
net_dispatch_msg(struct peer *p, const char *buf)
{
    uint32_t index, begin, length;
    int res = 0;

    switch (p->in.msg_num) {
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
                    || index >= p->n->tp->npieces
                    || !cm_has_piece(p->n->tp, index)
                    || begin + length > torrent_piece_size(p->n->tp, index))) {
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
        length = p->in.msg_len - 9;
        peer_on_piece(p, p->in.pc_index, p->in.pc_begin, length, buf);
        break;
    default:
        abort();
    }
    return res;
}

static int
net_mh_ok(struct peer *p)
{
    uint32_t mlen = p->in.msg_len;
    switch (p->in.msg_num) {
    case MSG_CHOKE:
    case MSG_UNCHOKE:
    case MSG_INTEREST:
    case MSG_UNINTEREST:
        return mlen == 1;
    case MSG_HAVE:
        return mlen == 5;
    case MSG_BITFIELD:
        return mlen == (uint32_t)ceil(p->n->tp->npieces / 8.0) + 1;
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
    if (p->in.state == BTP_MSGBODY && p->in.msg_num == MSG_PIECE) {
        p->n->downloaded += length;
        p->count_dwn += length;
    }
}

static int
net_state(struct peer *p, const char *buf)
{
    switch (p->in.state) {
    case SHAKE_PSTR:
        if (bcmp(buf, "\x13""BitTorrent protocol", 20) != 0)
            goto bad;
        peer_set_in_state(p, SHAKE_INFO, 20);
        break;
    case SHAKE_INFO:
        if (p->flags & PF_INCOMING) {
            struct torrent *tp = torrent_by_hash(buf);
            if (tp == NULL || !net_active(tp))
                goto bad;
            p->n = tp->net;
            peer_send(p, nb_create_shake(tp));
        } else if (bcmp(buf, p->n->tp->tl->hash, 20) != 0)
            goto bad;
        peer_set_in_state(p, SHAKE_ID, 20);
        break;
    case SHAKE_ID:
        if ((net_torrent_has_peer(p->n, buf)
             || bcmp(buf, btpd_get_peer_id(), 20) == 0))
            goto bad;
        bcopy(buf, p->id, 20);
        peer_on_shake(p);
        peer_set_in_state(p, BTP_MSGSIZE, 4);
        break;
    case BTP_MSGSIZE:
        p->in.msg_len = net_read32(buf);
        if (p->in.msg_len == 0)
            peer_on_keepalive(p);
        else
            peer_set_in_state(p, BTP_MSGHEAD, 1);
        break;
    case BTP_MSGHEAD:
        p->in.msg_num = buf[0];
        if (!net_mh_ok(p))
            goto bad;
        else if (p->in.msg_len == 1) {
            if (net_dispatch_msg(p, buf) != 0)
                goto bad;
            peer_set_in_state(p, BTP_MSGSIZE, 4);
        } else if (p->in.msg_num == MSG_PIECE)
            peer_set_in_state(p, BTP_PIECEMETA, 8);
        else
            peer_set_in_state(p, BTP_MSGBODY, p->in.msg_len - 1);
        break;
    case BTP_PIECEMETA:
        p->in.pc_index = net_read32(buf);
        p->in.pc_begin = net_read32(buf + 4);
        peer_set_in_state(p, BTP_MSGBODY, p->in.msg_len - 9);
        break;
    case BTP_MSGBODY:
        if (net_dispatch_msg(p, buf) != 0)
            goto bad;
        peer_set_in_state(p, BTP_MSGSIZE, 4);
        break;
    default:
        abort();
    }

    return 0;
bad:
    btpd_log(BTPD_L_CONN, "bad data from %p (%u, %u, %u).\n",
             p, p->in.state, p->in.msg_len, p->in.msg_num);
    peer_kill(p);
    return -1;
}

#define GRBUFLEN (1 << 15)

static unsigned long
net_read(struct peer *p, unsigned long rmax)
{
    size_t rest = p->in.buf != NULL ? p->in.st_bytes - p->in.off : 0;
    char buf[GRBUFLEN];
    struct iovec iov[2] = {
        {
            p->in.buf + p->in.off,
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
            p->in.off += nread;
            net_progress(p, nread);
            goto out;
        }
        net_progress(p, rest);
        if (net_state(p, p->in.buf) != 0)
            return nread;
        free(p->in.buf);
        p->in.buf = NULL;
        p->in.off = 0;
    }

    iov[1].iov_len = nread - rest;
    while (p->in.st_bytes <= iov[1].iov_len) {
        size_t consumed = p->in.st_bytes;
        net_progress(p, consumed);
        if (net_state(p, iov[1].iov_base) != 0)
            return nread;
        iov[1].iov_base += consumed;
        iov[1].iov_len -= consumed;
    }

    if (iov[1].iov_len > 0) {
        net_progress(p, iov[1].iov_len);
        p->in.off = iov[1].iov_len;
        p->in.buf = btpd_malloc(p->in.st_bytes);
        bcopy(iov[1].iov_base, p->in.buf, iov[1].iov_len);
    }

out:
    btpd_ev_add(&p->in_ev, NULL);
    return nread > 0 ? nread : 0;
}

int
net_connect2(struct sockaddr *sa, socklen_t salen, int *sd)
{
    if ((*sd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
        return errno;

    set_nonblocking(*sd);

    if (connect(*sd, sa, salen) == -1 && errno != EINPROGRESS) {
        int err = errno;
        btpd_log(BTPD_L_CONN, "Botched connection %s.\n", strerror(errno));
        close(*sd);
        return err;
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
    BTPDQ_FOREACH(tp, torrent_get_all(), entry) {
        unsigned long tp_up = 0, tp_dwn = 0;
        struct net *n = tp->net;
        struct peer *p;
        BTPDQ_FOREACH(p, &n->peers, p_entry) {
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
        n->rate_up += tp_up - compute_rate_sub(n->rate_up);
        n->rate_dwn += tp_dwn - compute_rate_sub(n->rate_dwn);
        tot_up += tp_up;
        tot_dwn += tp_dwn;
    }
    m_rate_up += tot_up - compute_rate_sub(m_rate_up);
    m_rate_dwn += tot_dwn - compute_rate_sub(m_rate_dwn);
}

static void
net_bw_tick(void)
{
    struct peer *p;

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
        while (((p = BTPDQ_FIRST(&net_bw_writeq)) != NULL
                   && m_bw_bytes_out > 0)) {
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

static void
run_peer_ticks(void)
{
    struct torrent *tp;
    struct peer *p, *next;

    BTPDQ_FOREACH_MUTABLE(p, &net_unattached, p_entry, next)
        peer_on_tick(p);

    BTPDQ_FOREACH(tp, torrent_get_all(), entry)
        BTPDQ_FOREACH_MUTABLE(p, &tp->net->peers, p_entry, next)
        peer_on_tick(p);
}

void
net_on_tick(void)
{
    run_peer_ticks();
    compute_rates();
    net_bw_tick();
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
    if (net_bw_limit_out == 0)
        net_write(p, 0);
    else if (m_bw_bytes_out > 0)
        m_bw_bytes_out -= net_write(p, m_bw_bytes_out);
    else {
        p->flags |= PF_ON_WRITEQ;
        BTPDQ_INSERT_TAIL(&net_bw_writeq, p, wq_entry);
    }
}

void
net_init(void)
{
    m_bw_bytes_out = net_bw_limit_out;
    m_bw_bytes_in = net_bw_limit_in;

    int safe_fds = min(getdtablesize(), FD_SETSIZE) * 4 / 5;
    if (net_max_peers == 0 || net_max_peers > safe_fds)
        net_max_peers = safe_fds;

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
    btpd_ev_add(&m_net_incoming, NULL);
}
