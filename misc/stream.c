#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "metainfo.h"
#include "subr.h"
#include "stream.h"

int
bts_open(struct bt_stream **res, struct metainfo *meta, fdcb_t fd_cb,
    void *fd_arg)
{
    struct bt_stream *bts = calloc(1, sizeof(*bts));
    if (bts == NULL)
        return ENOMEM;

    bts->meta = meta;
    bts->fd_cb = fd_cb;
    bts->fd_arg = fd_arg;
    bts->fd = -1;

    *res = bts;
    return 0;
}

int
bts_close(struct bt_stream *bts)
{
    int err = 0;
    if (bts->fd != -1 && close(bts->fd) == -1)
        err = errno;
    free(bts);
    return err;
}

int
bts_seek(struct bt_stream *bts, off_t off)
{
    if (bts->t_off == off)
        return 0;

    bts->t_off = off;

    struct fileinfo *files = bts->meta->files;
    unsigned i;
    for (i = 0; off >= files[i].length; i++)
        off -= files[i].length;

    if (i != bts->index) {
        if (bts->fd != -1) {
            if (close(bts->fd) == -1)
                return errno;
            bts->fd = -1;
        }
    } else if (bts->fd != -1)
        lseek(bts->fd, off, SEEK_SET);
    
    bts->index = i;
    bts->f_off = off;

    return 0;
}

int
bts_get(struct bt_stream *bts, off_t off, uint8_t *buf, size_t len)
{
    struct fileinfo *files = bts->meta->files;
    size_t boff, wantread;
    ssize_t didread;
    int err;

    assert(off + len <= bts->meta->total_length);
    if ((err = bts_seek(bts, off)) != 0)
        return err;

    boff = 0;
    while (boff < len) {
        if (bts->fd == -1) {
            err = bts->fd_cb(files[bts->index].path, &bts->fd, bts->fd_arg);
            if (err != 0)
                return err;
            if (bts->f_off != 0)
                lseek(bts->fd, bts->f_off, SEEK_SET);
        }

        wantread = min(len - boff, files[bts->index].length - bts->f_off);
        didread = read(bts->fd, buf + boff, wantread);
        if (didread == -1)
            return errno;

        boff += didread;
        bts->f_off += didread;
        bts->t_off += didread;
        if (bts->f_off == files[bts->index].length) {
            close(bts->fd);
            bts->fd = -1;
            bts->f_off = 0;
            bts->index++;
        }
        if (didread != wantread)
            return ENOENT;
    }
    return 0;
}

int
bts_put(struct bt_stream *bts, off_t off, const uint8_t *buf, size_t len)
{
    struct fileinfo *files = bts->meta->files;
    size_t boff, wantwrite;
    ssize_t didwrite;
    int err;

    assert(off + len <= bts->meta->total_length);
    if ((err = bts_seek(bts, off)) != 0)
        return err;

    boff = 0;
    while (boff < len) {
        if (bts->fd == -1) {
            err = bts->fd_cb(files[bts->index].path, &bts->fd, bts->fd_arg);
            if (err != 0)
                return err;
            if (bts->f_off != 0)
                lseek(bts->fd, bts->f_off, SEEK_SET);
        }

        wantwrite = min(len - boff, files[bts->index].length - bts->f_off);
        didwrite = write(bts->fd, buf + boff, wantwrite);
        if (didwrite == -1)
            return errno;

        boff += didwrite;
        bts->f_off += didwrite;
        bts->t_off += didwrite;
        if (bts->f_off == files[bts->index].length) {
            if (fsync(bts->fd) == -1) {
                int err = errno;
                close(bts->fd);
                return err;
            }
            if (close(bts->fd) == -1)
                return errno;
            bts->fd = -1;
            bts->f_off = 0;
            bts->index++;
        }
    }
    return 0;
}

#define SHAFILEBUF (1 << 15)

int
bts_sha(struct bt_stream *bts, off_t start, off_t length, uint8_t *hash)
{
    SHA_CTX ctx;
    char buf[SHAFILEBUF];
    size_t wantread;
    int err = 0;

    SHA1_Init(&ctx);
    while (length > 0) {
        wantread = min(length, SHAFILEBUF);
        if ((err = bts_get(bts, start, buf, wantread)) != 0)
            break;
        length -= wantread;
        start += wantread;
        SHA1_Update(&ctx, buf, wantread);
    }
    SHA1_Final(hash, &ctx);
    return err;
}

int
bts_hashes(struct metainfo *meta, fdcb_t fd_cb, hashcb_t cb, void *arg)
{
    int err = 0;
    uint8_t hash[SHA_DIGEST_LENGTH];
    uint32_t piece;
    struct bt_stream *bts;
    off_t plen = meta->piece_length;
    off_t llen = meta->total_length % plen;

    if ((err = bts_open(&bts, meta, fd_cb, arg)) != 0)
        return err;

    for (piece = 0; piece < meta->npieces; piece++) {
        off_t start = piece * plen;
        if (piece < meta->npieces - 1)
            err = bts_sha(bts, start, plen, hash);
        else
            err = bts_sha(bts, start, llen, hash);

        if (err == 0)
            cb(piece, hash, arg);
        else if (err == ENOENT) {
            cb(piece, NULL, arg);
            err = 0;
        } else
            break;
    }
    bts_close(bts);
    return err;
}
