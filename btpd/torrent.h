#ifndef BTPD_TORRENT_H
#define BTPD_TORRENT_H

#define PIECE_BLOCKLEN (1 << 14)

struct block {
    struct piece *pc;
    struct net_buf *msg;
    struct block_request_tq reqs;
};

struct piece {
    struct torrent *tp;

    uint32_t index;

    unsigned nreqs;

    unsigned nblocks;
    unsigned ngot;
    unsigned nbusy;
    unsigned next_block;

    struct block *blocks;

    const uint8_t *have_field;
    uint8_t *down_field;

    BTPDQ_ENTRY(piece) entry;
};

BTPDQ_HEAD(piece_tq, piece);

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

    struct content *cp;

    BTPDQ_ENTRY(torrent) entry;
    BTPDQ_ENTRY(torrent) net_entry;

    int net_active;

    uint8_t *busy_field;
    uint32_t npcs_busy;

    unsigned *piece_count;

    uint64_t uploaded, downloaded;

    unsigned long rate_up, rate_dwn;

    unsigned npeers;
    struct peer_tq peers;

    int endgame;
    struct piece_tq getlst;
};

BTPDQ_HEAD(torrent_tq, torrent);

int torrent_create(struct torrent **res, const char *path);
void torrent_activate(struct torrent *tp);
void torrent_deactivate(struct torrent *tp);

int torrent_has_peer(struct torrent *tp, const uint8_t *id);

off_t torrent_piece_size(struct torrent *tp, uint32_t index);
uint32_t torrent_block_size(struct piece *pc, uint32_t index);

enum cm_state {
    CM_STARTED,
    CM_STOPPED,
    CM_ERROR
};

void torrent_cm_cb(struct torrent *tp, enum cm_state state);

#endif
