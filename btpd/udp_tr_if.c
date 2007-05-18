#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <event.h>
#include <evdns.h>
#include <string.h>

#include "btpd.h"
#include "tracker_req.h"

struct udp_tr_req {
    enum { UDP_RESOLVE, UDP_CONN_SEND, UDP_CONN_RECV,
           UDP_ANN_SEND, UDP_ANN_RECV } state;
    int cancel;
    char *host;
    uint16_t port;
    uint32_t ip;
    struct event timer;
    BTPDQ_ENTRY(udp_tr_req) entry;
};

BTPDQ_HEAD(udp_req_tq, udp_tr_req);

static int m_sd;
static struct event m_recv;
static struct event m_send;
static struct udp_req_tq m_reqq = BTPDQ_HEAD_INITIALIZER(m_reqq);

static void
req_kill(struct udp_tr_req *req)
{
    free(req);
}

struct udp_tr_req *
udp_tr_req(struct torrent *tp, enum tr_event event, const char *aurl)
{
    return NULL;
}

void
udp_tr_cancel(struct udp_tr_req *req)
{
}

void
udp_tr_init(void)
{
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if ((m_sd = socket(PF_INET, SOCK_DGRAM, 0)) < 0)
        btpd_err("socket: %s\n", strerror(errno));

    if (bind(m_sd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        btpd_err("bind: %s\n", strerror(errno));

    event_set(&m_recv, m_sd, EV_READ, sock_cb, NULL);
    event_set(&m_send, m_sd, EV_WRITE, sock_cb, NULL);
}

static void
sock_cb(int sd, short type, void *arg)
{
}

static void
send_conn(struct udp_tr_req *req)
{
    req->state = UDP_CONN_SEND;
}

static void
udp_dnscb(int result, char type, int count, int ttl, void *addrs, void *arg)
{
    struct udp_tr_req *req = arg;
    if (req->cancel)
        req_kill(req);
    else if (result == DNS_ERR_NONE && type == DNS_IPv4_A && count > 0) {
        int addri = rand_between(0, count - 1);
        bcopy(addrs + addri * 4, &req->ip, 4);
        send_conn(req);
    } else {
        btpd_log(BTPD_L_ERROR, "failed to lookup '%s'.\n", req->host);
        tr_result(req->tp, TR_RES_FAIL, -1);
        req_kill(req);
    }
}
