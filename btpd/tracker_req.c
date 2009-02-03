#include "btpd.h"
#include "http_client.h"

#define REQ_DELAY 1
#define DEFAULT_INTERVAL rand_between(25 * 60, 30 * 60)
#define RETRY1_TIMEOUT (& (struct timespec) {240 + rand_between(0, 120), 0})
#define RETRY2_TIMEOUT (& (struct timespec) {900 + rand_between(0, 300), 0})

long tr_key;

static long m_tlast_req, m_tnext_req;

struct tr_entry {
    BTPDQ_ENTRY(tr_entry) entry;
    char *failure;
    char *url;
    enum tr_type type;
};

BTPDQ_HEAD(tr_entry_tq, tr_entry);

struct tr_tier {
    struct torrent *tp;
    struct tr_entry *cur;
    struct tr_entry_tq trackers;
    struct timeout timer;
    BTPDQ_ENTRY(tr_tier) entry;
    void *req;
    int interval;
    int bad_conns;
    int active;
    int has_responded;
    enum tr_event event;
};

BTPDQ_HEAD(tr_tier_tq, tr_tier);

struct trackers {
    struct tr_tier_tq trackers;
};

static struct tr_entry *
first_nonfailed(struct tr_tier *t)
{
    struct tr_entry *e;
    BTPDQ_FOREACH(e, &t->trackers, entry)
        if (e->failure == NULL)
            return e;
    abort();
}

static int
all_failed(struct tr_tier *t)
{
    struct tr_entry *e;
    BTPDQ_FOREACH(e, &t->trackers, entry)
        if (e->failure == NULL)
            return 0;
    return 1;
}

static void *
req_send(struct tr_tier *t)
{
    btpd_log(BTPD_L_TR, "sending event %d to '%s' for '%s'.\n",
        t->event, t->cur->url, torrent_name(t->tp));
    switch (t->cur->type) {
    case TR_HTTP:
        return httptr_req(t->tp, t, t->cur->url, t->event);
    default:
        abort();
    }
}

static void
req_cancel(struct tr_tier *t)
{
    switch (t->cur->type) {
    case TR_HTTP:
        httptr_cancel(t->req);
        break;
    default:
        abort();
    }
    t->req = NULL;
}

static void
entry_send(struct tr_tier *t, struct tr_entry *e, enum tr_event event)
{
    if (t->req != NULL)
        req_cancel(t);
    t->event = event;
    t->cur = e;
    if (m_tlast_req > btpd_seconds - REQ_DELAY) {
        m_tnext_req = max(m_tnext_req, m_tlast_req) + REQ_DELAY;
        btpd_timer_add(&t->timer,
            (& (struct timespec) { m_tnext_req - btpd_seconds, 0 }));
        return;
    }
    btpd_timer_del(&t->timer);
    if ((t->req = req_send(t)) == NULL)
        btpd_err("failed to create tracker message to '%s' (%s).",
            e->url, strerror(errno));
    m_tlast_req = btpd_seconds;
}

static int
tier_active(struct tr_tier *t)
{
    return t->active;
}

static void
tier_timer_cb(int fd, short type, void *arg)
{
    struct tr_tier *t = arg;
    assert(tier_active(t) && !all_failed(t));
    entry_send(t, first_nonfailed(t), t->event);
}

static void
tier_start(struct tr_tier *t)
{
    struct tr_entry *e;
    assert(!tier_active(t) || t->event == TR_EV_STOPPED);
    BTPDQ_FOREACH(e, &t->trackers, entry)
        if (e->failure != NULL) {
            free(e->failure);
            e->failure = NULL;
        }
    t->has_responded = 0;
    t->bad_conns = 0;
    t->active = 1;
    entry_send(t, BTPDQ_FIRST(&t->trackers), TR_EV_STARTED);
}

static void
tier_stop(struct tr_tier *t)
{
    if (!tier_active(t) || t->event == TR_EV_STOPPED)
        return;

    if (!t->has_responded && t->bad_conns > 1) {
        btpd_timer_del(&t->timer);
        if (t->req != NULL)
            req_cancel(t);
        t->active = 0;
    } else
        entry_send(t, first_nonfailed(t), TR_EV_STOPPED);
}

static void
tier_complete(struct tr_tier *t)
{
    if (tier_active(t) && t->event == TR_EV_EMPTY)
        entry_send(t, BTPDQ_FIRST(&t->trackers), TR_EV_COMPLETED);
}

static void
add_tracker(struct tr_tier *t, const char *url)
{
    struct tr_entry *e;
    struct http_url *hu;
    if ((hu = http_url_parse(url)) != NULL) {
        http_url_free(hu);
        e = btpd_calloc(1, sizeof(*e));
        if ((e->url = strdup(url)) == NULL)
            btpd_err("Out of memory.\n");
        e->type = TR_HTTP;
    } else {
        btpd_log(BTPD_L_TR, "skipping unsupported tracker '%s' for '%s'.\n",
            url, torrent_name(t->tp));
        return;
    }
    BTPDQ_INSERT_TAIL(&t->trackers, e, entry);
}

static struct tr_tier *
tier_create(struct torrent *tp, struct mi_tier *tier)
{
    struct tr_tier *t = btpd_calloc(1, sizeof(*t));
    t->tp = tp;
    BTPDQ_INIT(&t->trackers);
    for (int i = 0; i < tier->nurls; i++)
        add_tracker(t, tier->urls[i]);
    if (!BTPDQ_EMPTY(&t->trackers)) {
        t->interval = -1;
        t->event = TR_EV_STOPPED;
        evtimer_init(&t->timer, tier_timer_cb, t);
        return t;
    } else {
        free(t);
        return NULL;
    }
}

static void
tier_kill(struct tr_tier *t)
{
    struct tr_entry *e, *next;
    btpd_timer_del(&t->timer);
    if (t->req != NULL)
        req_cancel(t);
    BTPDQ_FOREACH_MUTABLE(e, &t->trackers, entry , next) {
        if (e->failure != NULL)
            free(e->failure);
        free(e->url);
        free(e);
    }
    free(t);
}

void
tr_create(struct torrent *tp, const char *mi)
{
    int i;
    struct tr_tier *t;
    struct mi_announce *ann;
    tp->tr = btpd_calloc(1, sizeof(*tp->tr));
    BTPDQ_INIT(&tp->tr->trackers);
    if ((ann = mi_announce(mi)) == NULL)
        btpd_err("Out of memory.\n");
    for (i = 0; i < ann->ntiers; i++)
        if ((t = tier_create(tp, &ann->tiers[i])) != NULL)
            BTPDQ_INSERT_TAIL(&tp->tr->trackers, t, entry);
    mi_free_announce(ann);
}

void
tr_kill(struct torrent *tp)
{
    struct tr_tier *t, *next;
    BTPDQ_FOREACH_MUTABLE(t, &tp->tr->trackers, entry, next)
        tier_kill(t);
    free(tp->tr);
    tp->tr = NULL;
}

void
tr_start(struct torrent *tp)
{
    struct tr_tier *t;
    BTPDQ_FOREACH(t, &tp->tr->trackers, entry)
        tier_start(t);
}

void
tr_stop(struct torrent *tp)
{
    struct tr_tier *t;
    BTPDQ_FOREACH(t, &tp->tr->trackers, entry)
        tier_stop(t);
}

void
tr_complete(struct torrent *tp)
{
    struct tr_tier *t;
    BTPDQ_FOREACH(t, &tp->tr->trackers, entry)
        tier_complete(t);
}

int
tr_active(struct torrent *tp)
{
    struct tr_tier *t;
    BTPDQ_FOREACH(t, &tp->tr->trackers, entry)
        if (tier_active(t))
            return 1;
    return 0;
}

int
tr_good_count(struct torrent *tp)
{
    int count = 0;
    struct tr_tier *t;
    BTPDQ_FOREACH(t, &tp->tr->trackers, entry)
        if (tier_active(t) && t->bad_conns == 0)
            count++;
    return count;
}

void
tr_result(struct tr_tier *t, struct tr_response *res)
{
    struct tr_entry *e;
    t->req = NULL;
    switch (res->type) {
    case TR_RES_FAIL:
        t->cur->failure = benc_str(res->mi_failure, NULL, NULL);
        btpd_log(BTPD_L_ERROR, "tracker at '%s' failed (%s).\n",
            t->cur->url, t->cur->failure);
        if (all_failed(t)) {
            t->active = 0;
            break;
        }
    case TR_RES_CONN:
        btpd_log(BTPD_L_TR, "connection to '%s' failed for '%s'.\n",
            t->cur->url, torrent_name(t->tp));
        e = t->cur;
        while ((e = BTPDQ_NEXT(e, entry)) != NULL && e->failure != NULL)
            ;
        if (e != NULL) {
            entry_send(t, e, t->event);
            break;
        }
        t->bad_conns++;
        if (t->event == TR_EV_STOPPED && t->bad_conns > 1)
            t->active = 0;
        else if (t->bad_conns == 1)
            entry_send(t, BTPDQ_FIRST(&t->trackers), t->event);
        else if (t->bad_conns == 2)
            btpd_timer_add(&t->timer, RETRY1_TIMEOUT);
        else
            btpd_timer_add(&t->timer, RETRY2_TIMEOUT);
        break;
    case TR_RES_BAD:
        btpd_log(BTPD_L_ERROR, "bad data from tracker '%s' for '%s'.\n",
            t->cur->url, torrent_name(t->tp));
    case TR_RES_OK:
        if (TR_RES_OK)
            btpd_log(BTPD_L_TR, "response from '%s' for '%s'.\n",
                t->cur->url, torrent_name(t->tp));
        if (t->event == TR_EV_STOPPED)
            t->active = 0;
        else {
            t->event = TR_EV_EMPTY;
            if (res->interval > 0)
                t->interval = res->interval;
            btpd_timer_add(&t->timer, (& (struct timespec) {
                t->interval > 0 ? t->interval : DEFAULT_INTERVAL, 0 }));
        }
        t->bad_conns = 0;
        t->has_responded = 1;
        BTPDQ_REMOVE(&t->trackers, t->cur, entry);
        BTPDQ_INSERT_HEAD(&t->trackers, t->cur, entry);
        break;
    default:
        abort();
    }
}

void
tr_init(void)
{
    tr_key = random();
}
