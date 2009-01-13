#include "btpd.h"

#include <http_client.h>
#include <iobuf.h>

#define MAX_DOWNLOAD (1 << 18)  // 256kB

static const char *m_tr_events[] = { "started", "stopped", "completed", "" };

struct http_tr_req {
    struct torrent *tp;
    struct http_req *req;
    struct iobuf buf;
    struct fdev ioev;
    nameconn_t nc;
    int sd;
    enum tr_event event;
};

static void
http_tr_free(struct http_tr_req *treq)
{
    if (treq->sd != -1) {
        btpd_ev_del(&treq->ioev);
        close(treq->sd);
    }
    iobuf_free(&treq->buf);
    free(treq);
}

static void
maybe_connect_to(struct torrent *tp, const char *pinfo)
{
    const char *pid;
    char *ip;
    int port;
    size_t len;

    if ((pid = benc_dget_mem(pinfo, "peer id", &len)) == NULL || len != 20)
        return;

    if (bcmp(btpd_get_peer_id(), pid, 20) == 0)
        return;

    if (net_torrent_has_peer(tp->net, pid))
        return;

    if ((ip = benc_dget_str(pinfo, "ip", NULL)) == NULL)
        return;

    port = benc_dget_int(pinfo, "port");
    peer_create_out(tp->net, pid, ip, port);

    if (ip != NULL)
        free(ip);
}

static int
parse_reply(struct torrent *tp, const char *content, size_t size, int parse,
    int *interval)
{
    const char *buf;
    size_t len;
    const char *peers;
    const char *v6key[] = {"peers6", "peers_ipv6"};

    if (benc_validate(content, size) != 0)
        goto bad_data;

    if ((buf = benc_dget_mem(content, "failure reason", &len)) != NULL) {
        btpd_log(BTPD_L_ERROR, "Tracker failure: '%.*s' for '%s'.\n",
            (int)len, buf, torrent_name(tp));
        return 1;
    }

    if (!parse) {
        *interval = -1;
        return 0;
    }

    if (!benc_dct_chk(content, 2, BE_INT, 1, "interval", BE_ANY, 1, "peers"))
        goto bad_data;

    *interval = benc_dget_int(content, "interval");
    if (*interval < 1)
        goto bad_data;

    peers = benc_dget_any(content, "peers");

    if (benc_islst(peers)) {
        for (peers = benc_first(peers);
             peers != NULL && net_npeers < net_max_peers;
             peers = benc_next(peers))
            maybe_connect_to(tp, peers);
    } else if (benc_isstr(peers)) {
        if (net_ipv4) {
            peers = benc_dget_mem(content, "peers", &len);
            for (size_t i = 0; i < len && net_npeers < net_max_peers; i += 6)
                peer_create_out_compact(tp->net, AF_INET, peers + i);
        }
    } else
        goto bad_data;

    if (!net_ipv6)
        return 0;
    for (int k = 0; k < 2; k++) {
        peers = benc_dget_any(content, v6key[k]);
        if (peers != NULL && benc_isstr(peers)) {
            peers = benc_dget_mem(content, v6key[k], &len);
            for (size_t i = 0; i < len && net_npeers < net_max_peers; i += 18)
                peer_create_out_compact(tp->net, AF_INET6, peers + i);
        }
    }
    return 0;

bad_data:
    btpd_log(BTPD_L_ERROR, "Bad data from tracker for '%s'.\n",
        torrent_name(tp));
    return 1;
}

static void
http_cb(struct http_req *req, struct http_response *res, void *arg)
{
    int interval;
    struct http_tr_req *treq = arg;
    switch (res->type) {
    case HTTP_T_ERR:
        btpd_log(BTPD_L_ERROR, "http request failed for '%s'.\n",
            torrent_name(treq->tp));
        tr_result(treq->tp, TR_RES_FAIL, -1);
        http_tr_free(treq);
        break;
    case HTTP_T_DATA:
        if (treq->buf.off + res->v.data.l > MAX_DOWNLOAD) {
            tr_result(treq->tp, TR_RES_FAIL, -1);
            http_tr_cancel(treq);
            break;
        }
        if (!iobuf_write(&treq->buf, res->v.data.p, res->v.data.l))
            btpd_err("Out of memory.\n");
        break;
    case HTTP_T_DONE:
        if (parse_reply(treq->tp, treq->buf.buf, treq->buf.off,
                treq->event != TR_EV_STOPPED, &interval) == 0)
            tr_result(treq->tp, TR_RES_OK, interval);
        else
            tr_result(treq->tp, TR_RES_FAIL, -1);
        http_tr_free(treq);
        break;
    default:
        break;
    }
}

static void
sd_io_cb(int sd, short type, void *arg)
{
    struct http_tr_req *treq = arg;
    switch (type) {
    case EV_READ:
        if (http_read(treq->req, sd) && !http_want_read(treq->req))
            btpd_ev_disable(&treq->ioev, EV_READ);
        break;
    case EV_WRITE:
        if (http_write(treq->req, sd) && !http_want_write(treq->req))
            btpd_ev_disable(&treq->ioev, EV_WRITE);
        break;
    default:
        abort();
    }
}

static void
nc_cb(void *arg, int error, int sd)
{
    struct http_tr_req *treq = arg;
    if (error) {
        tr_result(treq->tp, TR_RES_FAIL, -1);
        http_cancel(treq->req);
        http_tr_free(treq);
    } else {
        treq->sd = sd;
        uint16_t flags =
            (http_want_read(treq->req) ? EV_READ : 0) |
            (http_want_write(treq->req) ? EV_WRITE : 0);
        btpd_ev_new(&treq->ioev, sd, flags, sd_io_cb, treq);
    }
}

struct http_tr_req *
http_tr_req(struct torrent *tp, enum tr_event event, const char *aurl)
{
    char e_hash[61], e_id[61], url[512], qc;
    const uint8_t *peer_id = btpd_get_peer_id();
    struct http_url *http_url;

    qc = (strchr(aurl, '?') == NULL) ? '?' : '&';

    for (int i = 0; i < 20; i++)
        snprintf(e_hash + i * 3, 4, "%%%.2x", tp->tl->hash[i]);
    for (int i = 0; i < 20; i++)
        snprintf(e_id + i * 3, 4, "%%%.2x", peer_id[i]);

    snprintf(url, sizeof(url),
        "%s%cinfo_hash=%s&peer_id=%s&key=%ld%s%s&port=%d&uploaded=%llu"
        "&downloaded=%llu&left=%llu&compact=1%s%s",
        aurl, qc, e_hash, e_id, tr_key,
        tr_ip_arg == NULL ? "" : "&ip=", tr_ip_arg == NULL ? "" : tr_ip_arg,
        net_port, tp->net->uploaded, tp->net->downloaded,
        (long long)tp->total_length - cm_content(tp),
        event == TR_EV_EMPTY ? "" : "&event=", m_tr_events[event]);

    struct http_tr_req *treq = btpd_calloc(1, sizeof(*treq));
    if (!http_get(&treq->req, url, "User-Agent: " BTPD_VERSION "\r\n",
            http_cb, treq)) {
        free(treq);
        return NULL;
    }
    treq->buf = iobuf_init(4096);
    if (treq->buf.error)
        btpd_err("Out of memory.\n");
    treq->tp = tp;
    treq->event = event;
    treq->sd = -1;
    http_url = http_url_get(treq->req);
    treq->nc = btpd_name_connect(http_url->host, http_url->port, nc_cb, treq);
    return treq;
}

void
http_tr_cancel(struct http_tr_req *treq)
{
    if (treq->sd == -1)
        btpd_name_connect_cancel(treq->nc);
    http_cancel(treq->req);
    http_tr_free(treq);
}
