#include <stdio.h>
#include <string.h>

#include "btpd.h"
#include "benc.h"
#include "subr.h"
#include "http.h"
#include "tracker_req.h"

#define REQ_TIMEOUT (& (struct timeval) { 120, 0 })
#define RETRY_WAIT (& (struct timeval) { rand_between(35, 70), 0 })

enum tr_event {
    TR_EV_STARTED,
    TR_EV_STOPPED,
    TR_EV_COMPLETED,
    TR_EV_EMPTY
};

static const char *m_events[] = { "started", "stopped", "completed", "" };

enum timer_type {
    TIMER_NONE,
    TIMER_TIMEOUT,
    TIMER_INTERVAL,
    TIMER_RETRY
};

struct tracker {
    enum timer_type ttype;
    enum tr_event event;
    int interval;
    unsigned nerrors;
    struct http *req;
    struct event timer;
};

static void tr_send(struct torrent *tp, enum tr_event event);

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
parse_reply(struct torrent *tp, const char *content, size_t size, int parse)
{
    const char *buf;
    size_t len;
    const char *peers;
    int interval;

    if (benc_validate(content, size) != 0)
        goto bad_data;

    if ((buf = benc_dget_mem(content, "failure reason", &len)) != NULL) {
        btpd_log(BTPD_L_ERROR, "Tracker failure: '%.*s' for '%s'.\n",
            (int)len, buf, torrent_name(tp));
        return 1;
    }

    if (!parse)
        return 0;

    if (!benc_dct_chk(content, 2, BE_INT, 1, "interval", BE_ANY, 1, "peers"))
        goto bad_data;

    interval = benc_dget_int(content, "interval");
    if (interval < 1)
        goto bad_data;

    tp->tr->interval = interval;
    peers = benc_dget_any(content, "peers");

    if (benc_islst(peers)) {
        for (peers = benc_first(peers);
             peers != NULL && net_npeers < net_max_peers;
             peers = benc_next(peers))
            maybe_connect_to(tp, peers);
    } else if (benc_isstr(peers)) {
        peers = benc_dget_mem(content, "peers", &len);
        for (size_t i = 0; i < len && net_npeers < net_max_peers; i += 6)
            peer_create_out_compact(tp->net, peers + i);
    } else
        goto bad_data;

    return 0;

bad_data:
    btpd_log(BTPD_L_ERROR, "Bad data from tracker for '%s'.\n",
        torrent_name(tp));
    return 1;
}

static void
tr_set_stopped(struct torrent *tp)
{
    struct tracker *tr = tp->tr;
    event_del(&tr->timer);
    tr->ttype = TIMER_NONE;
    if (tr->req != NULL) {
        http_cancel(tr->req);
        tr->req = NULL;
    }
    torrent_on_tr_stopped(tp);
}

static void
http_cb(struct http *req, struct http_res *res, void *arg)
{
    struct torrent *tp = arg;
    struct tracker *tr = tp->tr;
    assert(tr->ttype == TIMER_TIMEOUT);
    tr->req = NULL;
    if (res->res == HRES_OK && parse_reply(tp, res->content, res->length,
            tr->event != TR_EV_STOPPED) == 0) {
        tr->nerrors = 0;
        tr->ttype = TIMER_INTERVAL;
        event_add(&tr->timer, (& (struct timeval) { tr->interval, 0 }));
    } else {
        tr->nerrors++;
        tr->ttype = TIMER_RETRY;
        event_add(&tr->timer, RETRY_WAIT);
    }
    if (tr->event == TR_EV_STOPPED && (tr->nerrors == 0 || tr->nerrors >= 5))
        tr_set_stopped(tp);
}

static void
timer_cb(int fd, short type, void *arg)
{
    struct torrent *tp = arg;
    struct tracker *tr = tp->tr;
    switch (tr->ttype) {
    case TIMER_TIMEOUT:
        btpd_log(BTPD_L_ERROR, "Tracker request timed out for '%s'.\n",
            torrent_name(tp));
        tr->nerrors++;
        if (tr->event == TR_EV_STOPPED && tr->nerrors >= 5) {
            tr_set_stopped(tp);
            break;
        }
    case TIMER_RETRY:
        tr_send(tp, tr->event);
        break;
    case TIMER_INTERVAL:
        tr_send(tp, TR_EV_EMPTY);
        break;
    default:
        abort();
    }
}

static void
tr_send(struct torrent *tp, enum tr_event event)
{
    char e_hash[61], e_id[61], qc;;
    const uint8_t *peer_id = btpd_get_peer_id();

    struct tracker *tr = tp->tr;
    tr->event = event;
    if (tr->ttype == TIMER_TIMEOUT)
        http_cancel(tr->req);
    tr->ttype = TIMER_TIMEOUT;
    event_add(&tr->timer, REQ_TIMEOUT);

    qc = (strchr(tp->meta.announce, '?') == NULL) ? '?' : '&';

    for (int i = 0; i < 20; i++)
        snprintf(e_hash + i * 3, 4, "%%%.2x", tp->meta.info_hash[i]);
    for (int i = 0; i < 20; i++)
        snprintf(e_id + i * 3, 4, "%%%.2x", peer_id[i]);

    http_get(&tr->req, http_cb, tp,
        "%s%cinfo_hash=%s&peer_id=%s&port=%d&uploaded=%ju"
        "&downloaded=%ju&left=%ju&compact=1%s%s",
        tp->meta.announce, qc, e_hash, e_id, net_port,
        (intmax_t)tp->net->uploaded, (intmax_t)tp->net->downloaded,
        (intmax_t)tp->meta.total_length - cm_get_size(tp),
        event == TR_EV_EMPTY ? "" : "&event=", m_events[event]);
}

int
tr_create(struct torrent *tp)
{
    if (strncmp(tp->meta.announce, "http://", sizeof("http://") - 1) != 0) {
        btpd_log(BTPD_L_ERROR,
            "btpd currently has no support for the protocol specified in "
            "'%s'.\n", tp->meta.announce);
        return EINVAL;
    }
    tp->tr = btpd_calloc(1, sizeof(*tp->tr));
    evtimer_set(&tp->tr->timer, timer_cb, tp);
    return 0;
}

void
tr_kill(struct torrent *tp)
{
    struct tracker *tr = tp->tr;
    tp->tr = NULL;
    event_del(&tr->timer);
    if (tr->req != NULL)
        http_cancel(tr->req);
    free(tr);
}

void
tr_start(struct torrent *tp)
{
    tr_send(tp, TR_EV_STARTED);
}

void
tr_refresh(struct torrent *tp)
{
    tr_send(tp, TR_EV_EMPTY);
}

void
tr_complete(struct torrent *tp)
{
    tr_send(tp, TR_EV_COMPLETED);
}

void
tr_stop(struct torrent *tp)
{
    if (tp->tr->event == TR_EV_STOPPED)
        tr_set_stopped(tp);
    else
        tr_send(tp, TR_EV_STOPPED);
}

int
tr_active(struct torrent *tp)
{
    return tp->tr->ttype != TIMER_NONE;
}

unsigned
tr_errors(struct torrent *tp)
{
    return tp->tr->nerrors;
}
