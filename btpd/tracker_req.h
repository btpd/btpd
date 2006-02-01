#ifndef TRACKER_REQ_H
#define TRACKER_REQ_H

int tr_start(struct torrent *tp);
void tr_stop(struct torrent *tp);
void tr_refresh(struct torrent *tp);
void tr_complete(struct torrent *tp);
void tr_destroy(struct torrent *tp);

#endif
