#ifndef TRACKER_REQ_H
#define TRACKER_REQ_H

enum tr_event {
    TR_EV_STARTED,
    TR_EV_STOPPED,
    TR_EV_COMPLETED,
    TR_EV_EMPTY
};

enum tr_res {
    TR_RES_OK,
    TR_RES_FAIL
};

extern long tr_key;

int tr_create(struct torrent *tp, const char *mi);
void tr_kill(struct torrent *tp);
void tr_start(struct torrent *tp);
void tr_stop(struct torrent *tp);
void tr_refresh(struct torrent *tp);
void tr_complete(struct torrent *tp);
unsigned tr_errors(struct torrent *tp);
int tr_active(struct torrent *tp);

void tr_result(struct torrent *tp, enum tr_res res, int interval);

struct http_tr_req;

struct http_tr_req *http_tr_req(struct torrent *tp, enum tr_event event,
    const char *aurl);
void http_tr_cancel(struct http_tr_req *treq);

#endif
