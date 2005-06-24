#ifndef TRACKER_REQ_H
#define TRACKER_REQ_H

enum tr_event {
    TR_STARTED = 1,
    TR_STOPPED,
    TR_COMPLETED,
    TR_EMPTY
};

void tracker_req(struct torrent *tp, enum tr_event tr_event);

#endif
