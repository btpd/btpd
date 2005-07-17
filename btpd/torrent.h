#ifndef BTPD_TORRENT_H
#define BTPD_TORRENT_H

struct piece {
    uint32_t index;
    unsigned nblocks;

    unsigned ngot;
    unsigned nbusy;

    uint8_t *have_field;
    uint8_t *down_field;

    BTPDQ_ENTRY(piece) entry;
};

BTPDQ_HEAD(piece_tq, piece);

struct torrent {
    const char *relpath;
    struct metainfo meta;

    BTPDQ_ENTRY(torrent) entry;

    void *imem;
    size_t isiz;

    uint8_t *piece_field;
    uint8_t *block_field;

    uint32_t have_npieces;
 
    unsigned long *piece_count;

    uint64_t uploaded, downloaded;
    
    unsigned long choke_time;
    unsigned long opt_time;
    unsigned long tracker_time;    

    short ndown;
    struct peer *optimistic;
    
    unsigned npeers;
    struct peer_tq peers;

    int endgame;
    struct piece_tq getlst;
};

BTPDQ_HEAD(torrent_tq, torrent);

off_t torrent_bytes_left(struct torrent *tp);

char *torrent_get_bytes(struct torrent *tp, off_t start, size_t len);
void torrent_put_bytes(struct torrent *tp, const char *buf,
		       off_t start, size_t len);

int torrent_load(const char *metafile);

void torrent_unload(struct torrent *tp);

int torrent_has_peer(struct torrent *tp, const uint8_t *id);

struct torrent *torrent_get_by_hash(const uint8_t *hash);

off_t torrent_piece_size(struct torrent *tp, uint32_t index);

#endif
