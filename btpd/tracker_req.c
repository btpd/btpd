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

void
maybe_connect_to(struct torrent *tp, const char *pinfo)
{
    const char *pid = NULL;
    char *ip = NULL;
    int64_t port;
    size_t len;

    if (!benc_isdct(pinfo))
        return;

    if (benc_dget_str(pinfo, "peer id", &pid, &len) != 0 || len != 20)
        return;

    if (bcmp(btpd_get_peer_id(), pid, 20) == 0)
        return;

    if (net_torrent_has_peer(tp->net, pid))
        return;

    if (benc_dget_strz(pinfo, "ip", &ip, NULL) != 0)
        goto out;

    if (benc_dget_int64(pinfo, "port", &port) != 0)
        goto out;

    peer_create_out(tp->net, pid, ip, port);

out:
    if (ip != NULL)
        free(ip);
}

static int
parse_reply(struct torrent *tp, const char *content, size_t size)
{
    char *buf;
    const char *peers;
    uint32_t interval;

    if (benc_validate(content, size) != 0 || !benc_isdct(content)) {
        btpd_log(BTPD_L_ERROR, "Bad data from tracker.\n");
        return 1;
    }

    if ((benc_dget_strz(content, "failure reason", &buf, NULL)) == 0) {
        btpd_log(BTPD_L_ERROR, "Tracker failure: %s.\n", buf);
        free(buf);
        return 1;
    }

    if ((benc_dget_uint32(content, "interval", &interval)) != 0) {
        btpd_log(BTPD_L_ERROR, "Bad data from tracker.\n");
        return 1;
    }

    tp->tr->interval = interval;
    btpd_log(BTPD_L_BTPD, "Got interval %d.\n", interval);

    int error = 0;
    size_t length;

    if ((error = benc_dget_lst(content, "peers", &peers)) == 0) {
        for (peers = benc_first(peers);
             peers != NULL && net_npeers < net_max_peers;
             peers = benc_next(peers))
            maybe_connect_to(tp, peers);
    }

    if (error == EINVAL) {
        error = benc_dget_str(content, "peers", &peers, &length);
        if (error == 0 && length % 6 == 0) {
            size_t i;
            for (i = 0; i < length && net_npeers < net_max_peers; i += 6)
                peer_create_out_compact(tp->net, peers + i);
        }
    }

    if (error != 0) {
        btpd_log(BTPD_L_ERROR, "Bad data from tracker.\n");
        return 1;
    }

    return 0;
}

static void
http_cb(struct http *req, struct http_res *res, void *arg)
{
    struct torrent *tp = arg;
    struct tracker *tr = tp->tr;
    assert(tr->ttype == TIMER_TIMEOUT);
    tr->req = NULL;
    if ((http_succeeded(res) &&
            parse_reply(tp, res->content, res->length) == 0)) {
        tr->nerrors = 0;
        tr->ttype = TIMER_INTERVAL;
        event_add(&tr->timer, (& (struct timeval) { tr->interval, 0 }));
    } else {
        tr->nerrors++;
        tr->ttype = TIMER_RETRY;
        event_add(&tr->timer, RETRY_WAIT);
    }
    if (tr->event == TR_EV_STOPPED && (tr->nerrors == 0 || tr->nerrors >= 5))
        tr_destroy(tp);
}

static void
timer_cb(int fd, short type, void *arg)
{
    struct torrent *tp = arg;
    struct tracker *tr = tp->tr;
    switch (tr->ttype) {
    case TIMER_TIMEOUT:
        tr->nerrors++;
        if (tr->event == TR_EV_STOPPED && tr->nerrors >= 5) {
            tr_destroy(tp);
            break;
        }
    case TIMER_RETRY:
        if (tr->event == TR_EV_STOPPED) {
            event_add(&tr->timer, REQ_TIMEOUT);
            http_redo(&tr->req);
        } else
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
tr_start(struct torrent *tp)
{
    assert(tp->tr == NULL);
    if (strncmp(tp->meta.announce, "http://", sizeof("http://") - 1) != 0) {
        btpd_log(BTPD_L_ERROR,
            "btpd currently has no support for the protocol specified in "
            "'%s'.\n", tp->meta.announce);
        return EINVAL;
    }

    struct tracker *tr = btpd_calloc(1, sizeof(*tr));
    evtimer_set(&tr->timer, timer_cb, tp);
    tp->tr = tr;

    tr_send(tp, TR_EV_STARTED);

    return 0;
}

void
tr_destroy(struct torrent *tp)
{
    struct tracker *tr = tp->tr;
    tp->tr = NULL;
    event_del(&tr->timer);
    if (tr->req != NULL)
        http_cancel(tr->req);
    free(tr);
    torrent_on_tr_stopped(tp);
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
    tr_send(tp, TR_EV_STOPPED);
}
