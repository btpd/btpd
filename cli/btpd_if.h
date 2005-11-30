#ifndef BTPD_IF_H
#define BTPD_IF_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

struct ipc {
    struct sockaddr_un addr;
};

int ipc_open(const char *key, struct ipc **out);
int ipc_close(struct ipc *ipc);

int btpd_add(struct ipc *ipc, char **path, unsigned npaths, char **out);
int btpd_del(struct ipc *ipc, uint8_t (*hash)[20],
             unsigned nhashes, char **out);
int btpd_die(struct ipc *ipc);
int btpd_stat(struct ipc *ipc, char **out);

#endif
