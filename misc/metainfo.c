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

void
print_metainfo(struct metainfo *tp)
{
    unsigned i;

    printf("Info hash: ");
    for (i = 0; i < 20; i++)
        printf("%.2x", tp->info_hash[i]);
    printf("\n");
    printf("Tracker URL: %s\n", tp->announce);
    printf("Piece length: %jd\n", (intmax_t)tp->piece_length);
    printf("Number of pieces: %u\n", tp->npieces);
    printf("Number of files: %u\n", tp->nfiles);
    printf("Advisory name: %s\n", tp->name);
    printf("Files:\n");
    for (i = 0; i < tp->nfiles; i++) {
        printf("%s (%jd)\n",
            tp->files[i].path, (intmax_t)tp->files[i].length);
    }
    printf("Total length: %jd\n\n", (intmax_t)tp->total_length);
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
    size_t npath, plen, len;
    const char *plst, *iter, *str;

    if (!benc_dct_chk(fdct, 2, BE_INT, 1, "length", BE_LST, 1, "path"))
        return EINVAL;

    tfp->length = benc_dget_int(fdct, "length");
    plst = benc_dget_lst(fdct, "path");

    npath = plen = 0;
    iter = benc_first(plst);
    while (iter != NULL) {
        if (!benc_isstr(iter))
            return EINVAL;
        str = benc_mem(iter, &len, &iter);
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
    str = benc_mem(iter, &len, &iter);
    memcpy(tfp->path, str, len);
    plen = len;
    npath--;
    while (npath > 0) {
        tfp->path[plen++] = '/';
        str = benc_mem(iter, &len, &iter);
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
    int err = 0;
    const char *base_addr = bep;
    const char *hash_addr;

    if (!benc_dct_chk(bep, 5,
            BE_STR, 1, "announce",
            BE_DCT, 1, "info",
            BE_INT, 2, "info", "piece length",
            BE_STR, 2, "info", "pieces",
            BE_STR, 2, "info", "name"))
        return EINVAL;

    if ((tp->announce = benc_dget_str(bep, "announce", NULL)) == NULL) {
        err = ENOMEM;
        goto out;
    }
    bep = benc_dget_dct(bep, "info");
    SHA1(bep, benc_length(bep), tp->info_hash);
    tp->piece_length = benc_dget_int(bep, "piece length");
    hash_addr = benc_dget_mem(bep, "pieces", &len);
    tp->npieces = len / 20;
    tp->pieces_off = hash_addr - base_addr;
    if (mem_hashes) {
        tp->piece_hash = (uint8_t (*)[20])benc_dget_mema(bep, "pieces", NULL);
        if (tp->piece_hash == NULL) {
            err = ENOMEM;
            goto out;
        }
    }
    tp->name = benc_dget_str(bep, "name", NULL);

    if (benc_dct_chk(bep, 1, BE_INT, 1, "length")) {
        tp->total_length = benc_dget_int(bep, "length");
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
    } else if (benc_dct_chk(bep, 1, BE_LST, 1, "files")) {
        int i;
        const char *flst, *fdct;

        flst = benc_dget_lst(bep, "files");
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
