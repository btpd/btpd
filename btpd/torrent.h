#ifndef BTPD_TORRENT_H
#define BTPD_TORRENT_H

#define PIECE_BLOCKLEN (1 << 14)
#define RELPATH_SIZE 41

enum torrent_state {
    T_STARTING,
    T_ACTIVE,
    T_STOPPING
};

struct torrent {
    char relpath[RELPATH_SIZE];
    struct metainfo meta;

    enum torrent_state state;

    struct content *cm;
    struct tracker *tr;
    struct net *net;

    BTPDQ_ENTRY(torrent) entry;
};

BTPDQ_HEAD(torrent_tq, torrent);

unsigned torrent_count(void);
const struct torrent_tq *torrent_get_all(void);
struct torrent *torrent_get(const uint8_t *hash);

int torrent_start(const uint8_t *hash);
void torrent_stop(struct torrent *tp);
int torrent_set_links(const uint8_t *hash, const char *torrent,
    const char *content);

off_t torrent_piece_size(struct torrent *tp, uint32_t piece);
uint32_t torrent_piece_blocks(struct torrent *tp, uint32_t piece);
uint32_t torrent_block_size(struct torrent *tp, uint32_t piece,
    uint32_t nblocks, uint32_t block);

void torrent_on_cm_stopped(struct torrent *tp);
void torrent_on_cm_started(struct torrent *tp);
void torrent_on_tr_stopped(struct torrent *tp);

#endif
