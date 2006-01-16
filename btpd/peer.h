#ifndef BTPD_PEER_H
#define BTPD_PEER_H

#define PF_I_WANT         0x1   /* We want to download from the peer */
#define PF_I_CHOKE        0x2   /* We choke the peer */
#define PF_P_WANT         0x4   /* The peer wants to download from us */
#define PF_P_CHOKE        0x8   /* The peer is choking us */
#define PF_ON_READQ      0x10
#define PF_ON_WRITEQ     0x20
#define PF_ATTACHED      0x40
#define PF_NO_REQUESTS   0x80
#define PF_INCOMING     0x100
#define PF_DO_UNWANT    0x200

#define MAXPIECEMSGS 128
#define MAXPIPEDREQUESTS 10

void peer_set_in_state(struct peer *p, enum input_state state, size_t size);

void peer_send(struct peer *p, struct net_buf *nb);
int peer_unsend(struct peer *p, struct nb_link *nl);
void peer_sent(struct peer *p, struct net_buf *nb);

void peer_unchoke(struct peer *p);
void peer_choke(struct peer *p);
void peer_unwant(struct peer *p, uint32_t index);
void peer_want(struct peer *p, uint32_t index);
void peer_request(struct peer *p, struct block_request *req);
void peer_cancel(struct peer *p, struct block_request *req,
    struct net_buf *nb);

int peer_requested(struct peer *p, struct block *blk);

void peer_create_in(int sd);
void peer_create_out(struct net *n, const uint8_t *id,
    const char *ip, int port);
void peer_create_out_compact(struct net *n, const char *compact);
void peer_kill(struct peer *p);

void peer_on_no_reqs(struct peer *p);
void peer_on_keepalive(struct peer *p);
void peer_on_shake(struct peer *p);
void peer_on_interest(struct peer *p);
void peer_on_uninterest(struct peer *p);
void peer_on_choke(struct peer *p);
void peer_on_unchoke(struct peer *p);
void peer_on_have(struct peer *p, uint32_t index);
void peer_on_bitfield(struct peer *p, const uint8_t *field);
void peer_on_piece(struct peer *p, uint32_t index, uint32_t begin,
    uint32_t length, const char *data);
void peer_on_request(struct peer *p, uint32_t index, uint32_t begin,
    uint32_t length);
void peer_on_cancel(struct peer *p, uint32_t index, uint32_t begin,
    uint32_t length);

int peer_active_down(struct peer *p);
int peer_active_up(struct peer *p);
int peer_chokes(struct peer *p);
int peer_wanted(struct peer *p);
int peer_laden(struct peer *p);
int peer_has(struct peer *p, uint32_t index);
int peer_leech_ok(struct peer *p);
int peer_full(struct peer *p);

#endif
