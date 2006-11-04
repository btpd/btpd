#include <string.h>

#include "btpd.h"
#include "subr.h"
#include "tracker_req.h"

#define REQ_DELAY 1
#define STOP_ERRORS 5
#define REQ_TIMEOUT (& (struct timeval) { 120, 0 })
#define RETRY_WAIT (& (struct timeval) { rand_between(35, 70), 0 })

long tr_key;

static long m_tlast_req, m_tnext_req;

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
    int tier, url;
    struct mi_announce *ann;
    void *req;
    struct event timer;
};

typedef struct _dummy *(*request_fun_t)(struct torrent *, enum tr_event,
    const char *);
typedef void (*cancel_fun_t)(struct _dummy *);

struct tr_op {
    int len;
    const char *scheme;
    request_fun_t request;
    cancel_fun_t cancel;
};

static struct tr_op m_http_op = {
    7, "http://", (request_fun_t)http_tr_req, (cancel_fun_t)http_tr_cancel
};

static struct tr_op *m_tr_ops[] = {
    &m_http_op, NULL
};

static char *
get_url(struct tracker *tr)
{
    return tr->ann->tiers[tr->tier].urls[tr->url];
}

static void
good_url(struct tracker *tr)
{
    char *set = tr->ann->tiers[tr->tier].urls[tr->url], *hold;
    for (int i = 0; i <= tr->url; i++) {
        hold = tr->ann->tiers[tr->tier].urls[i];
        tr->ann->tiers[tr->tier].urls[i] = set;
        set = hold;
    }
    tr->tier = 0;
    tr->url = 0;
}

static void
next_url(struct tracker *tr)
{
    tr->url = (tr->url + 1) % tr->ann->tiers[tr->tier].nurls;
    if (tr->url == 0)
        tr->tier = (tr->tier + 1) % tr->ann->ntiers;
}

struct tr_op *
get_op(struct tracker *tr)
{
    struct tr_op *op;
    char *url = get_url(tr);
    for (op = m_tr_ops[0]; op != NULL; op++)
        if (strncasecmp(op->scheme, url, op->len) == 0)
            return op;
    return NULL;
}

static void
tr_cancel(struct tracker *tr)
{
    struct tr_op *op = get_op(tr);
    assert(op != NULL);
    op->cancel(tr->req);
    tr->req = NULL;
}

static void
tr_send(struct torrent *tp, enum tr_event event)
{
    struct tracker *tr = tp->tr;
    struct tr_op *op = get_op(tr);

    tr->event = event;
    if (tr->req != NULL)
        tr_cancel(tr);

    if (m_tlast_req > btpd_seconds - REQ_DELAY) {
        m_tnext_req = max(m_tnext_req, m_tlast_req) + REQ_DELAY;
        tr->ttype = TIMER_RETRY;
        btpd_ev_add(&tr->timer,
            (& (struct timeval) { m_tnext_req - btpd_seconds, 0 }));
        return;
    }

    if ((op == NULL ||
            (tr->req = op->request(tp, event, get_url(tr))) == NULL)) {
        next_url(tr);
        tr->ttype = TIMER_RETRY;
        btpd_ev_add(&tr->timer, (& (struct timeval) { 20, 0 }));
    } else {
        m_tlast_req = btpd_seconds;
        tr->ttype = TIMER_TIMEOUT;
        btpd_ev_add(&tr->timer, REQ_TIMEOUT);
    }
}

static void
tr_set_stopped(struct torrent *tp)
{
    struct tracker *tr = tp->tr;
    btpd_ev_del(&tr->timer);
    tr->ttype = TIMER_NONE;
    if (tr->req != NULL)
        tr_cancel(tr);
}

void
tr_result(struct torrent *tp, enum tr_res res, int interval)
{
    struct tracker *tr = tp->tr;
    tr->req = NULL;
    if (tr->event == TR_EV_STOPPED &&
            (res == TR_RES_OK || tr->nerrors >= STOP_ERRORS - 1))
        tr_set_stopped(tp);
    else if (res == TR_RES_OK) {
        good_url(tr);
        tr->interval = interval;
        tr->nerrors = 0;
        tr->ttype = TIMER_INTERVAL;
        btpd_ev_add(&tr->timer, (& (struct timeval) { tr->interval, 0}));
    } else {
        tr->nerrors++;
        tr->ttype = TIMER_RETRY;
        btpd_ev_add(&tr->timer, RETRY_WAIT);
        next_url(tr);
    }
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
        if (tr->event == TR_EV_STOPPED && tr->nerrors >= STOP_ERRORS) {
            tr_set_stopped(tp);
            break;
        }
        tr_cancel(tr);
        next_url(tr);
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

int
tr_create(struct torrent *tp, const char *mi)
{
    tp->tr = btpd_calloc(1, sizeof(*tp->tr));
    if ((tp->tr->ann = mi_announce(mi)) == NULL)
        btpd_err("Out of memory.\n");
    evtimer_set(&tp->tr->timer, timer_cb, tp);
    return 0;
}

void
tr_kill(struct torrent *tp)
{
    struct tracker *tr = tp->tr;
    tp->tr = NULL;
    btpd_ev_del(&tr->timer);
    if (tr->req != NULL)
        tr_cancel(tr);
    mi_free_announce(tr->ann);
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

void
tr_init(void)
{
    tr_key = random();
}
