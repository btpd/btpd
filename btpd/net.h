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

struct iob_link {
    BTPDQ_ENTRY(iob_link) entry;
    void (*kill_buf)(struct io_buffer *);
    struct io_buffer iob;
};

BTPDQ_HEAD(io_tq, iob_link);

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

#define MAX_INPUT_LEFT 12

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
void net_by_second(void);

struct peer;

void net_send_uninterest(struct peer *p);
void net_send_interest(struct peer *p);
void net_send_unchoke(struct peer *p);
void net_send_choke(struct peer *p);

void net_send_have(struct peer *p, uint32_t index);
void net_send_request(struct peer *p, struct piece_req *req);
void net_send_cancel(struct peer *p, struct piece_req *req);

void net_handshake(struct peer *p, int incoming);

void net_read_cb(int sd, short type, void *arg);
void net_write_cb(int sd, short type, void *arg);
int net_connect(const char *ip, int port, int *sd);

void net_unsend_piece(struct peer *p, struct piece_req *req);

#endif
