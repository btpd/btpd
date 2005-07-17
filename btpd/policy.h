#ifndef BTPD_POLICY_H
#define BTPD_POLICY_H

void cm_by_second(struct torrent *tp);

void cm_on_new_peer(struct peer *peer);
void cm_on_lost_peer(struct peer *peer);

void cm_on_choke(struct peer *peer);
void cm_on_unchoke(struct peer *peer);
void cm_on_upload(struct peer *peer);
void cm_on_unupload(struct peer *peer);
void cm_on_interest(struct peer *peer);
void cm_on_uninterest(struct peer *peer);
void cm_on_download(struct peer *peer);
void cm_on_undownload(struct peer *peer);
void cm_on_piece_ann(struct peer *peer, uint32_t piece);
void cm_on_block(struct peer *peer);

void cm_schedule_piece(struct torrent *tp);
int cm_assign_requests(struct peer *peer, int nreqs);
	    
void cm_unassign_requests(struct peer *peer);

#endif
