#ifndef BTPD_IF_H
#define BTPD_IF_H

struct ipc;

enum torrent_state { //XXX: Same as in btpd/torrent.h
    T_STARTING,
    T_ACTIVE,
    T_STOPPING
};

enum ipc_code {
    IPC_OK,
    IPC_FAIL,
    IPC_ERROR,
    IPC_COMMERR
};

struct btstat {
    unsigned ntorrents;
    struct tpstat {
        uint8_t *hash;
        char *name;
        enum torrent_state state;
        unsigned tr_errors;
        unsigned peers;
        uint32_t pieces_got, pieces_seen, torrent_pieces;
        off_t content_got, content_size;
        unsigned long long downloaded, uploaded;
        unsigned long rate_up, rate_down;
    } torrents[];
};

int ipc_open(const char *dir, struct ipc **out);
int ipc_close(struct ipc *ipc);

enum ipc_code btpd_add(struct ipc *ipc, const uint8_t *hash,
    const char *torrent, const char *content);
enum ipc_code btpd_del(struct ipc *ipc, const uint8_t *hash);
enum ipc_code btpd_die(struct ipc *ipc, int seconds);
enum ipc_code btpd_stat(struct ipc *ipc, struct btstat **out);
void free_btstat(struct btstat *stat);

#endif
