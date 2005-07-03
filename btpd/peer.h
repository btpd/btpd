#ifndef BTPD_PEER_H
#define BTPD_PEER_H

#define PF_I_WANT	0x01	/* We want to download from the peer */
#define PF_I_CHOKE	0x02	/* We choke the peer */
#define	PF_P_WANT	0x04	/* The peer wants to download from us */
#define	PF_P_CHOKE	0x08	/* The peer is choking us */
#define PF_ON_READQ	0x10
#define PF_ON_WRITEQ	0x20
#define PF_ATTACHED	0x40
#define PF_WRITE_CLOSE	0x80	/* Close connection after writing all data */

#define RATEHISTORY 20

struct peer {
    int sd;
    uint8_t flags;
    uint8_t *piece_field;
    uint32_t npieces;
    unsigned nwant;

    uint8_t id[20];

    struct torrent *tp;

    struct piece_req_tq p_reqs, my_reqs;

    struct io_tq outq;

    struct event in_ev;
    struct event out_ev;

    struct input_reader *reader;

    unsigned long rate_to_me[RATEHISTORY];
    unsigned long rate_from_me[RATEHISTORY];

    BTPDQ_ENTRY(peer) cm_entry;

    BTPDQ_ENTRY(peer) rq_entry;
    BTPDQ_ENTRY(peer) wq_entry;
};

BTPDQ_HEAD(peer_tq, peer);

void peer_unchoke(struct peer *p);
void peer_choke(struct peer *p);
void peer_unwant(struct peer *p, uint32_t index);
void peer_want(struct peer *p, uint32_t index);
void peer_request(struct peer *p, uint32_t index,
		  uint32_t begin, uint32_t len);
void peer_cancel(struct peer *p, uint32_t index, uint32_t begin, uint32_t len);

void peer_have(struct peer *p, uint32_t index);

unsigned long peer_get_rate(unsigned long *rates);

void peer_create_in(int sd);
void peer_create_out(struct torrent *tp, const uint8_t *id,
		     const char *ip, int port);
void peer_create_out_compact(struct torrent *tp, const char *compact);
void peer_kill(struct peer *p);

#endif
