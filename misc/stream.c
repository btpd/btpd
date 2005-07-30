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

struct bt_stream_ro *
bts_open_ro(struct metainfo *meta, off_t off, F_fdcb fd_cb, void *fd_arg)
{
    struct bt_stream_ro *bts = malloc(sizeof(*bts));
    if (bts == NULL)
	return NULL;

    bts->meta = meta;
    bts->fd_cb = fd_cb;
    bts->fd_arg = fd_arg;
    bts->t_off = 0;
    bts->f_off = 0;
    bts->index = 0;
    bts->fd = -1;
    bts_seek_ro(bts, off);
    return bts;
}

void
bts_seek_ro(struct bt_stream_ro *bts, off_t off)
{
    struct fileinfo *files = bts->meta->files;

    assert(off >= 0 && off <= bts->meta->total_length);

    if (bts->fd != -1) {
	close(bts->fd);
	bts->fd = -1;
    }

    bts->t_off = off;
    bts->index = 0;

    while (off >= files[bts->index].length) {
	off -= files[bts->index].length;
	bts->index++;
    }

    bts->f_off = off;
}

int
bts_read_ro(struct bt_stream_ro *bts, char *buf, size_t len)
{
    struct fileinfo *files = bts->meta->files;
    size_t boff, wantread;
    ssize_t didread;

    assert(bts->t_off + len <= bts->meta->total_length);

    boff = 0;
    while (boff < len) {
	if (bts->fd == -1) {
	    int err =
		bts->fd_cb(files[bts->index].path, &bts->fd, bts->fd_arg);
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

void
bts_close_ro(struct bt_stream_ro *bts)
{
    if (bts->fd != -1)
	close(bts->fd);
    free(bts);
}

#define SHAFILEBUF (1 << 15)

int
bts_sha(struct bt_stream_ro *bts, off_t length, uint8_t *hash)
{
    SHA_CTX ctx;
    char buf[SHAFILEBUF];
    size_t wantread;
    int err = 0;

    SHA1_Init(&ctx);
    while (length > 0) {
	wantread = min(length, SHAFILEBUF);
	if ((err = bts_read_ro(bts, buf, wantread)) != 0)
	    break;
	length -= wantread;
	SHA1_Update(&ctx, buf, wantread);
    }
    SHA1_Final(hash, &ctx);
    return err;
}

int
bts_hashes(struct metainfo *meta,
    F_fdcb fd_cb,
    void (*cb)(uint32_t, uint8_t *, void *),
    void *arg)
{
    int err = 0;
    uint8_t hash[SHA_DIGEST_LENGTH];
    uint32_t piece;
    struct bt_stream_ro *bts;
    off_t plen = meta->piece_length;
    off_t llen = meta->total_length % plen;

    if ((bts = bts_open_ro(meta, 0, fd_cb, arg)) == NULL)
	return ENOMEM;
    
    for (piece = 0; piece < meta->npieces; piece++) {	
        if (piece < meta->npieces - 1)
	    err = bts_sha(bts, plen, hash);
	else
	    err = bts_sha(bts, llen, hash);

	if (err == 0)
	    cb(piece, hash, arg);
	else if (err == ENOENT) {
	    cb(piece, NULL, arg);
	    if (piece < meta->npieces - 1)
		bts_seek_ro(bts, (piece + 1) * plen);
	    err = 0;
	} else
	    break;
    }
    bts_close_ro(bts);
    return err;
}

struct bt_stream_wo *
bts_open_wo(struct metainfo *meta, off_t off, F_fdcb fd_cb, void *fd_arg)
{
    struct bt_stream_wo *bts = malloc(sizeof(*bts));
    if (bts == NULL)
	return NULL;

    bts->meta = meta;
    bts->fd_cb = fd_cb;
    bts->fd_arg = fd_arg;
    bts->t_off = 0;
    bts->f_off = 0;
    bts->index = 0;
    bts->fd = -1;
    bts_seek_ro((struct bt_stream_ro *)bts, off);
    return bts;
}

int
bts_write_wo(struct bt_stream_wo *bts, const char *buf, size_t len)
{
    struct fileinfo *files = bts->meta->files;
    size_t boff, wantwrite;
    ssize_t didwrite;

    assert(bts->t_off + len <= bts->meta->total_length);

    boff = 0;
    while (boff < len) {
	if (bts->fd == -1) {
	    int err =
		bts->fd_cb(files[bts->index].path, &bts->fd, bts->fd_arg);
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

int
bts_close_wo(struct bt_stream_wo *bts)
{
    int err = 0;
    if (bts->fd != -1) {
	if (fsync(bts->fd) == -1) {
	    err = errno;
	    close(bts->fd);
	} else if (close(bts->fd) == -1)
	    err = errno;
    }
    free(bts);
    return err;
}
