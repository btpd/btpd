#ifndef BTPD_TORRENT_H
#define BTPD_TORRENT_H

#define PIECE_BLOCKLEN (1 << 14)

enum torrent_state {
    T_INACTIVE,
    T_STARTING,
    T_ACTIVE,
    T_STOPPING
};

struct torrent {
    const char *relpath;
    struct metainfo meta;

    enum torrent_state state;

    struct content *cm;
    struct tracker *tr;
    struct net *net;

    BTPDQ_ENTRY(torrent) entry;
};

BTPDQ_HEAD(torrent_tq, torrent);

int torrent_create(struct torrent **res, const char *path);
void torrent_activate(struct torrent *tp);
void torrent_deactivate(struct torrent *tp);

off_t torrent_piece_size(struct torrent *tp, uint32_t index);
uint32_t torrent_block_size(struct piece *pc, uint32_t index);

enum cm_state {
    CM_STARTED,
    CM_STOPPED,
    CM_ERROR
};

void torrent_cm_cb(struct torrent *tp, enum cm_state state);

#endif
