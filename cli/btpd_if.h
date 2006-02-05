#ifndef BTPD_IF_H
#define BTPD_IF_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

struct ipc;

enum ipc_code {
    IPC_OK,
    IPC_FAIL,
    IPC_COMMERR
};

struct btstat {
    unsigned ntorrents;
    struct tpstat {
        char *name;
        unsigned num;
        char state;

        unsigned errors;
        unsigned npeers;
        uint32_t npieces, nseen;
        off_t have, total;
        long long downloaded, uploaded;
        unsigned long rate_up, rate_down;
    } torrents[];
};

int ipc_open(const char *dir, struct ipc **out);
int ipc_close(struct ipc *ipc);

enum ipc_code btpd_die(struct ipc *ipc, int seconds);
enum ipc_code btpd_stat(struct ipc *ipc, struct btstat **out);

enum ipc_code btpd_del_num(struct ipc *ipc, unsigned num);
enum ipc_code btpd_start_num(struct ipc *ipc, unsigned num);
enum ipc_code btpd_stop_num(struct ipc *ipc, unsigned num);

void free_btstat(struct btstat *stat);

#endif
