#ifndef BTPD_POLICY_H
#define BTPD_POLICY_H

// policy_choke.c

void choke_alg(struct torrent *tp);
void next_optimistic(struct torrent *tp, struct peer *np);

// policy_subr.c

int piece_full(struct piece *pc);
void piece_free(struct piece *pc);

void cm_on_piece_unfull(struct piece *pc);
void cm_on_piece(struct piece *pc);

struct piece *cm_new_piece(struct torrent *tp, uint32_t index);
struct piece *cm_find_piece(struct torrent *tp, uint32_t index);
unsigned cm_piece_assign_requests(struct piece *pc, struct peer *p);
void cm_piece_assign_requests_eg(struct piece *pc, struct peer *p);
unsigned  cm_assign_requests(struct peer *p);
void cm_assign_requests_eg(struct peer *p);
void cm_unassign_requests(struct peer *p);
void cm_unassign_requests_eg(struct peer *p);

// policy_if.c

void cm_by_second(struct torrent *tp);

void cm_on_new_peer(struct peer *p);
void cm_on_lost_peer(struct peer *p);

void cm_on_choke(struct peer *p);
void cm_on_unchoke(struct peer *p);
void cm_on_upload(struct peer *p);
void cm_on_unupload(struct peer *p);
void cm_on_interest(struct peer *p);
void cm_on_uninterest(struct peer *p);
void cm_on_download(struct peer *p);
void cm_on_undownload(struct peer *p);
void cm_on_piece_ann(struct peer *p, uint32_t index);
void cm_on_block(struct peer *p, uint32_t index, uint32_t begin,
    uint32_t length, const char *data);

void cm_on_ok_piece(struct piece *pc);
void cm_on_bad_piece(struct piece *pc);

#endif
