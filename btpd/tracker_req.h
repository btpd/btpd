#ifndef TRACKER_REQ_H
#define TRACKER_REQ_H

int tr_create(struct torrent *tp);
void tr_kill(struct torrent *tp);
void tr_start(struct torrent *tp);
void tr_stop(struct torrent *tp);
void tr_refresh(struct torrent *tp);
void tr_complete(struct torrent *tp);
unsigned tr_errors(struct torrent *tp);
int tr_active(struct torrent *tp);

#endif
