#ifndef BTPD_NET_H
#define BTPD_NET_H

#define MSG_CHOKE	0
#define MSG_UNCHOKE	1
#define MSG_INTEREST	2
#define MSG_UNINTEREST	3
#define MSG_HAVE	4
#define MSG_BITFIELD	5
#define MSG_REQUEST	6
#define MSG_PIECE	7
#define MSG_CANCEL	8

#define NB_CHOKE	0
#define NB_UNCHOKE	1
#define NB_INTEREST	2
#define NB_UNINTEREST	3
#define NB_HAVE		4
#define NB_BITFIELD	5
#define NB_REQUEST	6
#define NB_PIECE	7
#define NB_CANCEL	8
#define NB_TORRENTDATA	10
#define NB_MULTIHAVE	11
#define NB_BITDATA	12
#define NB_SHAKE	13

struct net_buf {
    unsigned refs;

    struct {
	short type;
	uint32_t index, begin, length;
    } info;

    char *buf;
    size_t len;
    void (*kill_buf)(char *, size_t);
};

struct nb_link {
    struct net_buf *nb;
    BTPDQ_ENTRY(nb_link) entry;
};

BTPDQ_HEAD(nb_tq, nb_link);

struct net_buf *nb_create_alloc(short type, size_t len);
struct net_buf *nb_create_set(short type, char *buf, size_t len,
    void (*kill_buf)(char *, size_t));
int nb_drop(struct net_buf *nb);
void nb_hold(struct net_buf *nb);

struct peer;

struct input_reader {
    unsigned long (*read)(struct peer *, unsigned long);
    void (*kill)(struct input_reader *);
};

struct bitfield_reader {
    struct input_reader rd;
    struct io_buffer iob;
};

struct piece_reader {
    struct input_reader rd;
    struct io_buffer iob;
    uint32_t index;
    uint32_t begin;
};

#define SHAKE_LEN 68

enum shake_state {
    SHAKE_INIT,
    SHAKE_PSTR,
    SHAKE_RESERVED,
    SHAKE_INFO,
    SHAKE_ID
};

struct handshake {
    struct input_reader rd;
    enum shake_state state;
    int incoming;
    struct io_buffer in;
    char _io_buf[SHAKE_LEN];
};

#define MAX_INPUT_LEFT 16

struct generic_reader {
    struct input_reader rd;
    struct io_buffer iob;
    char _io_buf[MAX_INPUT_LEFT];
};

struct piece_req {
    uint32_t index, begin, length;
    struct iob_link *head; /* Pointer to outgoing piece. */
    BTPDQ_ENTRY(piece_req) entry;
};

BTPDQ_HEAD(piece_req_tq, piece_req);

void net_connection_cb(int sd, short type, void *arg);
void net_bw_rate(void);
void net_bw_cb(int sd, short type, void *arg);

struct peer;

void net_send_uninterest(struct peer *p);
void net_send_interest(struct peer *p);
void net_send_unchoke(struct peer *p);
void net_send_choke(struct peer *p);

void net_send_have(struct peer *p, uint32_t index);
void net_send_request(struct peer *p, struct piece_req *req);
void net_send_piece(struct peer *p, uint32_t index, uint32_t begin,
    char *block, size_t blen);
void net_send_cancel(struct peer *p, struct piece_req *req);

void net_handshake(struct peer *p, int incoming);

void net_read_cb(int sd, short type, void *arg);
void net_write_cb(int sd, short type, void *arg);
int net_connect2(struct sockaddr *sa, socklen_t salen, int *sd);
int net_connect(const char *ip, int port, int *sd);

void net_unsend_piece(struct peer *p, struct piece_req *req);

#endif
