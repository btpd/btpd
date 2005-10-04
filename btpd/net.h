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

#define WRITE_TIMEOUT (& (struct timeval) { 60, 0 })

#define SHAKE_LEN 68

enum shake_state {
    SHAKE_INIT,
    SHAKE_PSTR,
    SHAKE_RESERVED,
    SHAKE_INFO,
    SHAKE_ID,
    NET_MSGSIZE,
    NET_MSGHEAD,
    NET_MSGBODY,
    NET_MSGPIECE
};

void net_set_state(struct peer *p, int state, size_t size);

void net_connection_cb(int sd, short type, void *arg);
void net_bw_rate(void);
void net_bw_cb(int sd, short type, void *arg);

void net_read_cb(int sd, short type, void *arg);
void net_write_cb(int sd, short type, void *arg);

int net_connect2(struct sockaddr *sa, socklen_t salen, int *sd);
int net_connect(const char *ip, int port, int *sd);

void net_write32(void *buf, uint32_t num);
uint32_t net_read32(void *buf);

#endif
