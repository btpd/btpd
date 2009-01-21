#ifndef TRACKER_REQ_H
#define TRACKER_REQ_H

enum tr_event {
    TR_EV_STARTED,
    TR_EV_STOPPED,
    TR_EV_COMPLETED,
    TR_EV_EMPTY
};

extern long tr_key;

enum tr_type { TR_HTTP };

struct tr_response {
    enum {
        TR_RES_FAIL, TR_RES_CONN, TR_RES_BAD, TR_RES_OK
    } type;
    const char *mi_failure;
    int interval;
};

struct tr_tier;

void tr_create(struct torrent *tp, const char *mi);
void tr_kill(struct torrent *tp);
void tr_start(struct torrent *tp);
void tr_stop(struct torrent *tp);
void tr_complete(struct torrent *tp);
int tr_active(struct torrent *tp);
void tr_result(struct tr_tier *t, struct tr_response *res);
int tr_good_count(struct torrent *tp);

struct httptr_req *httptr_req(struct torrent *tp, struct tr_tier *tr,
    const char *url, enum tr_event event);
void httptr_cancel(struct httptr_req *req);

#endif
