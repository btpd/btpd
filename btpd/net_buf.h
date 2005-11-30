#ifndef BTPD_NET_BUF_H
#define BTPD_NET_BUF_H

#define NB_CHOKE        0
#define NB_UNCHOKE      1
#define NB_INTEREST     2
#define NB_UNINTEREST   3
#define NB_HAVE         4
#define NB_BITFIELD     5
#define NB_REQUEST      6
#define NB_PIECE        7
#define NB_CANCEL       8
#define NB_TORRENTDATA  10
#define NB_MULTIHAVE    11
#define NB_BITDATA      12
#define NB_SHAKE        13

struct net_buf {
    short type;
    unsigned refs;
    char *buf;
    size_t len;
    void (*kill_buf)(char *, size_t);
};

struct nb_link {
    struct net_buf *nb;
    BTPDQ_ENTRY(nb_link) entry;
};

BTPDQ_HEAD(nb_tq, nb_link);

struct torrent;
struct peer;

struct net_buf *nb_create_piece(uint32_t index, uint32_t begin, size_t blen);
struct net_buf *nb_create_torrentdata(char *block, size_t blen);
struct net_buf *nb_create_request(uint32_t index,
    uint32_t begin, uint32_t length);
struct net_buf *nb_create_cancel(uint32_t index,
    uint32_t begin, uint32_t length);
struct net_buf *nb_create_have(uint32_t index);
struct net_buf *nb_create_multihave(struct torrent *tp);
struct net_buf *nb_create_unchoke(void);
struct net_buf *nb_create_choke(void);
struct net_buf *nb_create_uninterest(void);
struct net_buf *nb_create_interest(void);
struct net_buf *nb_create_bitfield(struct torrent *tp);
struct net_buf *nb_create_bitdata(struct torrent *tp);
struct net_buf *nb_create_shake(struct torrent *tp);

int nb_drop(struct net_buf *nb);
void nb_hold(struct net_buf *nb);

uint32_t nb_get_index(struct net_buf *nb);
uint32_t nb_get_begin(struct net_buf *nb);
uint32_t nb_get_length(struct net_buf *nb);

#endif
