#ifndef BTPD_STREAM_H
#define BTPD_STREAM_H

typedef int (*fdcb_t)(const char *, int *, void *);
typedef void (*hashcb_t)(uint32_t, uint8_t *, void *);

struct bt_stream {
    unsigned nfiles;
    struct mi_file *files;
    off_t totlen;
    fdcb_t fd_cb;
    void *fd_arg;
    unsigned index;
    off_t t_off;
    off_t f_off;
    int fd;
};

int bts_open(struct bt_stream **res, unsigned nfiles, struct mi_file *files,
    fdcb_t fd_cb, void *fd_arg);
int bts_close(struct bt_stream *bts);
int bts_get(struct bt_stream *bts, off_t off, uint8_t *buf, size_t len);
int bts_put(struct bt_stream *bts, off_t off, const uint8_t *buf, size_t len);
int bts_sha(struct bt_stream *bts, off_t start, off_t length, uint8_t *hash);

#endif
