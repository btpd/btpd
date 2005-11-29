#ifndef BTPD_UPLOAD_H
#define BTPD_UPLOAD_H

void ul_on_new_peer(struct peer *p);
void ul_on_lost_peer(struct peer *p);
void ul_on_lost_torrent(struct torrent *tp);
void ul_on_interest(struct peer *p);
void ul_on_uninterest(struct peer *p);
void ul_init(void);

#endif
