#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

#include "sha1.h"
#include "metainfo.h"
#include "subr.h"
#include "stream.h"

int
bts_open(struct bt_stream **res, unsigned nfiles, struct mi_file *files,
    fdcb_t fd_cb, void *fd_arg)
{
    struct bt_stream *bts = calloc(1, sizeof(*bts));
    if (bts == NULL)
        return ENOMEM;

    bts->nfiles = nfiles;
    bts->files = files;
    bts->fd_cb = fd_cb;
    bts->fd_arg = fd_arg;
    bts->fd = -1;

    for (unsigned i = 0; i < bts->nfiles; i++)
        bts->totlen += bts->files[i].length;

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

    unsigned i;
    for (i = 0; off >= bts->files[i].length; i++)
        off -= bts->files[i].length;

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
    size_t boff, wantread;
    ssize_t didread;
    int err;

    assert(off + len <= bts->totlen);
    if ((err = bts_seek(bts, off)) != 0)
        return err;

    boff = 0;
    while (boff < len) {
        if (bts->fd == -1) {
            while (bts->files[bts->index].length == 0)
                bts->index++;
            err = bts->fd_cb(bts->files[bts->index].path,
                &bts->fd, bts->fd_arg);
            if (err != 0)
                return err;
            if (bts->f_off != 0)
                lseek(bts->fd, bts->f_off, SEEK_SET);
        }

        wantread = min(len - boff, bts->files[bts->index].length - bts->f_off);
        didread = read(bts->fd, buf + boff, wantread);
        if (didread == -1)
            return errno;

        boff += didread;
        bts->f_off += didread;
        bts->t_off += didread;
        if (bts->f_off == bts->files[bts->index].length) {
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
    size_t boff, wantwrite;
    ssize_t didwrite;
    int err;

    assert(off + len <= bts->totlen);
    if ((err = bts_seek(bts, off)) != 0)
        return err;

    boff = 0;
    while (boff < len) {
        if (bts->fd == -1) {
            while (bts->files[bts->index].length == 0)
                bts->index++;
            err = bts->fd_cb(bts->files[bts->index].path,
                &bts->fd, bts->fd_arg);
            if (err != 0)
                return err;
            if (bts->f_off != 0)
                lseek(bts->fd, bts->f_off, SEEK_SET);
        }

        wantwrite = min(len - boff, bts->files[bts->index].length - bts->f_off);
        didwrite = write(bts->fd, buf + boff, wantwrite);
        if (didwrite == -1)
            return errno;

        boff += didwrite;
        bts->f_off += didwrite;
        bts->t_off += didwrite;
        if (bts->f_off == bts->files[bts->index].length) {
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
    struct sha1 ctx;
    char buf[SHAFILEBUF];
    size_t wantread;
    int err = 0;

    sha1_init(&ctx);
    while (length > 0) {
        wantread = min(length, SHAFILEBUF);
        if ((err = bts_get(bts, start, buf, wantread)) != 0)
            break;
        length -= wantread;
        start += wantread;
        sha1_update(&ctx, buf, wantread);
    }
    sha1_sum(&ctx, hash);
    return err;
}

const char *
bts_filename(struct bt_stream *bts)
{
    return bts->files[bts->index].path;
}
