#ifndef BTPD_TORRENT_H
#define BTPD_TORRENT_H

#define PIECE_BLOCKLEN (1 << 14)
#define RELPATH_SIZE SHAHEXSIZE

enum torrent_state {
    T_STARTING,
    T_LEECH,
    T_SEED,
    T_STOPPING
};

struct torrent {
    struct tlib *tl;

    enum torrent_state state;
    int delete;

    struct content *cm;
    struct trackers *tr;
    struct net *net;

    off_t total_length;
    off_t piece_length;
    uint32_t npieces;
    unsigned nfiles;
    struct mi_file *files;
    size_t pieces_off;

    BTPDQ_ENTRY(torrent) entry;
};

BTPDQ_HEAD(torrent_tq, torrent);

unsigned torrent_count(void);
const struct torrent_tq *torrent_get_all(void);
struct torrent *torrent_by_num(unsigned num);
struct torrent *torrent_by_hash(const uint8_t *hash);

enum ipc_err torrent_start(struct tlib *tl);
void torrent_stop(struct torrent *tp, int delete);

off_t torrent_piece_size(struct torrent *tp, uint32_t piece);
uint32_t torrent_piece_blocks(struct torrent *tp, uint32_t piece);
uint32_t torrent_block_size(struct torrent *tp, uint32_t piece,
    uint32_t nblocks, uint32_t block);
const char *torrent_name(struct torrent *tp);

void torrent_on_tick_all(void);

#endif
