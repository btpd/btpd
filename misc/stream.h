#ifndef BTPD_STREAM_H
#define BTPD_STREAM_H

typedef int (*F_fdcb)(const char *, int *, void *);

#define def_stream(name) \
struct name {\
    struct metainfo *meta;\
    F_fdcb fd_cb;\
    void *fd_arg;\
    unsigned index;\
    off_t t_off;\
    off_t f_off;\
    int fd;\
}

def_stream(bt_stream_ro);

struct bt_stream_ro *
bts_open_ro(struct metainfo *meta, off_t off, F_fdcb fd_cb, void *fd_arg);
int bts_read_ro(struct bt_stream_ro *bts, char *buf, size_t len);
void bts_seek_ro(struct bt_stream_ro *bts, off_t nbytes);
void bts_close_ro(struct bt_stream_ro *bts);

def_stream(bt_stream_wo);

struct bt_stream_wo *
bts_open_wo(struct metainfo *meta, off_t off, F_fdcb fd_cb, void *fd_arg);
int bts_write_wo(struct bt_stream_wo *bts, const char *buf, size_t len);
int bts_close_wo(struct bt_stream_wo *bts);

int bts_sha(struct bt_stream_ro *bts, off_t length, uint8_t *hash);
int bts_hashes(struct metainfo *, F_fdcb fd_cb,
	       void (*cb)(uint32_t, uint8_t *, void *), void *arg);

#endif
