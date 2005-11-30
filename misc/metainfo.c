#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "benc.h"
#include "metainfo.h"
#include "subr.h"

/*
 * d
 * announce = url
 * info = d
 *   name = advisory file/dir save name
 *   piece length = power of two length of each block
 *   pieces = 20b of sha1-hash * num of pieces
 *   length = length of file in bytes in single file download
 *   files = l d
 *     length = length of file in bytes
 *     path = l path components
 *
 */

#ifndef PRId64
#define PRId64 "lld"
#endif
#ifndef PRIu32
#define PRIu32 "u"
#endif

void
print_metainfo(struct metainfo *tp)
{
    unsigned i;

    printf("Info hash: ");
    for (i = 0; i < 20; i++)
        printf("%.2x", tp->info_hash[i]);
    printf("\n");
    printf("Tracker URL: %s\n", tp->announce);
    printf("Piece length: %" PRId64 "\n", (int64_t)tp->piece_length);
    printf("Number of pieces: %" PRIu32 "\n", tp->npieces);
    printf("Number of files: %u\n", tp->nfiles);
    printf("Advisory name: %s\n", tp->name);
    printf("Files:\n");
    for (i = 0; i < tp->nfiles; i++) {
        printf("%s (%" PRId64 ")\n",
            tp->files[i].path, (int64_t)tp->files[i].length);
    }
    printf("Total length: %" PRId64 "\n\n", (int64_t)tp->total_length);
}

static int
check_path(const char *path, size_t len)
{
    if (len == 0)
        return 0;
    else if (len == 1 && path[0] == '.')
        return 0;
    else if (len == 2 && path[0] == '.' && path[1] == '.')
        return 0;
    else if (memchr(path, '/', len) != NULL)
        return 0;
    return 1;
}

int
fill_fileinfo(const char *fdct, struct fileinfo *tfp)
{
    int err;
    size_t npath, plen, len;
    const char *plst, *iter, *str;

    if ((err = benc_dget_off(fdct, "length", &tfp->length)) != 0)
        return err;

    if ((err = benc_dget_lst(fdct, "path", &plst)) != 0)
        return err;

    npath = plen = 0;
    iter = benc_first(plst);
    while (iter != NULL) {
        if (!benc_isstr(iter))
            return EINVAL;
        benc_str(iter, &str, &len, &iter);
        if (!check_path(str, len))
            return EINVAL;
        npath++;
        plen += len;
    }
    if (npath == 0)
        return EINVAL;

    if ((tfp->path = malloc(plen + (npath - 1) + 1)) == NULL)
        return ENOMEM;

    iter = benc_first(plst);
    benc_str(iter, &str, &len, &iter);
    memcpy(tfp->path, str, len);
    plen = len;
    npath--;
    while (npath > 0) {
        tfp->path[plen++] = '/';
        benc_str(iter, &str, &len, &iter);
        memcpy(tfp->path + plen, str, len);
        plen += len;
        npath--;
    }
    tfp->path[plen] = '\0';
    return 0;
}

void
clear_metainfo(struct metainfo *mip)
{
    int i;
    if (mip->piece_hash != NULL)
        free(mip->piece_hash);
    if (mip->announce != NULL)
        free(mip->announce);
    if (mip->files != NULL) {
        for (i = 0; i < mip->nfiles; i++) {
            if (mip->files[i].path != NULL)
                free(mip->files[i].path);
        }
        free(mip->files);
    }
    if (mip->name != NULL)
        free(mip->name);
}

int
fill_metainfo(const char *bep, struct metainfo *tp, int mem_hashes)
{
    size_t len;
    int err;
    const char *base_addr = bep;
    const char *hash_addr;

    if (!benc_isdct(bep))
        return EINVAL;

    if ((err = benc_dget_strz(bep, "announce", &tp->announce, NULL)) != 0)
        goto out;

    if ((err = benc_dget_dct(bep, "info", &bep)) != 0)
        goto out;

    SHA1(bep, benc_length(bep), tp->info_hash);

    if ((err = benc_dget_off(bep, "piece length", &tp->piece_length)) != 0)
        goto out;

    if ((err = benc_dget_str(bep, "pieces", &hash_addr, &len)) != 0)
        goto out;

    if (len % 20 != 0) {
        err = EINVAL;
        goto out;
    }
    tp->npieces = len / 20;

    tp->pieces_off = hash_addr - base_addr;

    if (mem_hashes) {
        if ((tp->piece_hash = malloc(len)) == NULL) {
            err = ENOMEM;
            goto out;
        }
        bcopy(hash_addr, tp->piece_hash, len);
    }

    if ((err = benc_dget_strz(bep, "name", &tp->name, NULL)) != 0)
        goto out;

    err = benc_dget_off(bep, "length", &tp->total_length);
    if (err == 0) {
        tp->nfiles = 1;
        tp->files = calloc(1, sizeof(struct fileinfo));
        if (tp->files != NULL) {
            tp->files[0].length = tp->total_length;
            tp->files[0].path = strdup(tp->name);
            if (tp->files[0].path == NULL) {
                err = ENOMEM;
                goto out;
            }
        } else {
            err = ENOMEM;
            goto out;
        }
    }
    else if (err == ENOENT) {
        int i;
        const char *flst, *fdct;

        if ((err = benc_dget_lst(bep, "files", &flst)) != 0)
            goto out;

        tp->nfiles = benc_nelems(flst);
        if (tp->nfiles < 1) {
            err = EINVAL;
            goto out;
        }
        tp->files = calloc(tp->nfiles, sizeof(struct fileinfo));

        tp->total_length = 0;
        i = 0;
        for (fdct = benc_first(flst); fdct != NULL; fdct = benc_next(fdct)) {
            if (!benc_isdct(fdct)) {
                err = EINVAL;
                goto out;
            }

            if ((err = fill_fileinfo(fdct, &tp->files[i])) != 0)
                goto out;

            tp->total_length += tp->files[i].length;
            i++;
        }
    }
    else
        goto out;
out:
    if (err != 0)
        clear_metainfo(tp);

    return err;
}

int
load_metainfo(const char *path, off_t size, int mem_hashes,
              struct metainfo **res)
{
    char *buf;
    int fd, err = 0;

    if ((fd = open(path, O_RDONLY)) == -1)
        return errno;

    if (size <= 0) {
        struct stat sb;
        if (fstat(fd, &sb) == -1) {
            close(fd);
            return errno;
        } else
            size = sb.st_size;
    }

    if ((buf = mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
        err = errno;
    close(fd);

    if (err == 0)
        err = benc_validate(buf, size);

    if (err == 0)
        if ((*res = calloc(1, sizeof(**res))) == NULL)
            err = ENOMEM;

    if (err == 0)
        if ((err = fill_metainfo(buf, *res, mem_hashes)) != 0)
            free(*res);

    munmap(buf, size);
    return err;
}
