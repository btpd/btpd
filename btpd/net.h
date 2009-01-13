#ifndef BTPD_NET_H
#define BTPD_NET_H

#define MSG_CHOKE       0
#define MSG_UNCHOKE     1
#define MSG_INTEREST    2
#define MSG_UNINTEREST  3
#define MSG_HAVE        4
#define MSG_BITFIELD    5
#define MSG_REQUEST     6
#define MSG_PIECE       7
#define MSG_CANCEL      8

#define RATEHISTORY 20

extern struct peer_tq net_unattached;
extern struct peer_tq net_bw_readq;
extern struct peer_tq net_bw_writeq;
extern unsigned net_npeers;

void net_init(void);

void net_on_tick(void);

void net_create(struct torrent *tp);
void net_kill(struct torrent *tp);

void net_start(struct torrent *tp);
void net_stop(struct torrent *tp);
int net_active(struct torrent *tp);

int net_torrent_has_peer(struct net *n, const uint8_t *id);

void net_io_cb(int sd, short type, void *arg);

int net_connect_addr(int family, struct sockaddr *sa, socklen_t salen,
    int *sd);
int net_connect_name(const char *ip, int port, int *sd);

int net_af_spec(void);

#endif
