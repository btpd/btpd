#include <sys/types.h>

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

#include "benc.h"
#include "metainfo.h"
#include "subr.h"

/*
 * d
 * announce = url
 * announce-list = l l url ... e ... e
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

uint8_t *
mi_hashes(const char *p)
{
    return benc_dget_mema(benc_dget_dct(p, "info"), "pieces", NULL);
}

size_t
mi_npieces(const char *p)
{
    size_t plen;
    benc_dget_mem(benc_dget_dct(p, "info"), "pieces", &plen);
    return plen / 20;
}

int
mi_simple(const char *p)
{
    return benc_dget_lst(benc_dget_dct(p, "info"), "files") == NULL;
}

void
mi_free_announce(struct mi_announce *ann)
{
    if (ann->tiers != NULL) {
        for (int ti = 0; ti < ann->ntiers; ti++)
            if (ann->tiers[ti].urls != NULL) {
                for (int ui = 0; ui < ann->tiers[ti].nurls; ui++)
                    if (ann->tiers[ti].urls[ui] != NULL)
                        free(ann->tiers[ti].urls[ui]);
                free(ann->tiers[ti].urls);
            }
        free(ann->tiers);
    }
    free(ann);
}

static void
mi_shuffle_announce(struct mi_announce *ann)
{
    for (int i = 0; i < ann->ntiers; i++) {
        for (int j = 0; j < ann->tiers[i].nurls - 1; j++) {
            char *tmp = ann->tiers[i].urls[j];
            int ri = rand_between(j, ann->tiers[i].nurls - 1);
            ann->tiers[i].urls[j] = ann->tiers[i].urls[ri];
            ann->tiers[i].urls[ri] = tmp;
        }
    }
}

struct mi_announce *
mi_announce(const char *p)
{
    int ti, ui;
    const char *alst, *ulst, *url;
    struct mi_announce *res;

    if ((res = calloc(1, sizeof(*res))) == NULL)
        return NULL;

    if ((alst = benc_dget_lst(p, "announce-list")) != NULL) {
        res->ntiers = benc_nelems(alst);
        if ((res->tiers = calloc(res->ntiers, sizeof(*res->tiers))) == NULL)
            goto error;
        ti = 0; ulst = benc_first(alst);
        while (ulst != NULL) {
            res->tiers[ti].nurls = benc_nelems(ulst);
            res->tiers[ti].urls =
                calloc(res->tiers[ti].nurls, sizeof(*res->tiers[ti].urls));
            if (res->tiers[ti].urls == NULL)
                goto error;

            ui = 0; url = benc_first(ulst);
            while (url != NULL) {
                if ((res->tiers[ti].urls[ui] =
                        benc_str(url, NULL, NULL)) == NULL)
                    goto error;
                ui++; url = benc_next(url);
            }

            ti++; ulst = benc_next(ulst);
        }
    } else {
        res->ntiers = 1;
        if ((res->tiers = calloc(1, sizeof(*res->tiers))) == NULL)
            goto error;
        res->tiers[0].nurls = 1;
        if ((res->tiers[0].urls =
                calloc(1, sizeof(*res->tiers[0].urls))) == NULL)
            goto error;
        if ((res->tiers[0].urls[0] =
                benc_dget_str(p, "announce", NULL)) == NULL)
            goto error;
    }
    mi_shuffle_announce(res);
    return res;

error:
    if (res != NULL)
        mi_free_announce(res);
    return NULL;
}

off_t
mi_piece_length(const char *p)
{
    return benc_dget_int(benc_dget_dct(p, "info"), "piece length");
}

off_t
mi_total_length(const char *p)
{
    const char *info = benc_dget_dct(p, "info");
    const char *files = benc_dget_lst(info, "files");
    if (files != NULL) {
        off_t length = 0;
        const char *fdct = benc_first(files);
        while (fdct != NULL) {
            length += benc_dget_int(fdct, "length");
            fdct = benc_next(fdct);
        }
        return length;
    } else
        return benc_dget_int(info, "length");
}

uint8_t *
mi_info_hash(const char *p, uint8_t *hash)
{
    const char *info = benc_dget_dct(p, "info");
    if (hash == NULL)
        if ((hash = malloc(20)) == NULL)
            return NULL;
    return SHA1(info, benc_length(info), hash);
}

char *
mi_name(const char *p)
{
    return benc_dget_str(benc_dget_dct(p, "info"), "name", NULL);
}

size_t
mi_nfiles(const char *p)
{
    const char *files = benc_dget_lst(benc_dget_dct(p, "info"), "files");
    if (files != NULL)
        return benc_nelems(files);
    else
        return 1;
}

static char *
mi_filepath(const char *plst)
{
    char *res = NULL;
    const char *str;
    size_t npaths = 0, plen = 0, len;
    const char *iter = benc_first(plst);

    while (iter != NULL) {
        benc_mem(iter, &len, &iter);
        npaths++;
        plen += len;
    }

    if ((res = malloc(plen + (npaths - 1) + 1)) == NULL)
        return NULL;

    iter = benc_first(plst);
    str = benc_mem(iter, &len, &iter);
    bcopy(str, res, len);
    plen = len;
    npaths--;
    while (npaths > 0) {
        res[plen] = '/';
        plen++;
        str = benc_mem(iter, &len, &iter);
        bcopy(str, res + plen, len);
        plen += len;
        npaths--;
    }
    res[plen] = '\0';
    return res;
}

void
mi_free_files(unsigned nfiles, struct mi_file *files)
{
    for (unsigned i = 0; i < nfiles; i++)
        if (files[i].path != NULL)
            free(files[i].path);
    free(files);
}

struct mi_file *
mi_files(const char *p)
{
    struct mi_file *fi;
    const char *info = benc_dget_dct(p, "info");
    const char *files = benc_dget_lst(info, "files");
    if (files != NULL) {
        int i = 0;
        unsigned nfiles = benc_nelems(files);
        const char *fdct = benc_first(files);
        if ((fi = calloc(nfiles, sizeof(*fi))) == NULL)
            return NULL;
        for (fdct = benc_first(files); fdct != NULL; fdct = benc_next(fdct)) {
            fi[i].length = benc_dget_int(fdct, "length");
            fi[i].path = mi_filepath(benc_dget_lst(fdct, "path"));
            if (fi[i].path == NULL) {
                mi_free_files(nfiles, fi);
                return NULL;
            }
            i++;
        }
    } else {
        if ((fi = calloc(1, sizeof(*fi))) == NULL)
            return NULL;
        fi[0].length = benc_dget_int(info, "length");
        fi[0].path = benc_dget_str(info, "name", NULL);
        if (fi[0].path == NULL) {
            free(fi);
            return NULL;
        }
    }
    return fi;
}

static int
mi_test_path(const char *path, size_t len)
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

static int
mi_test_files(const char *files)
{
    int fcount = 0;
    const char *fdct = benc_first(files);
    while (fdct != NULL) {
        const char *plst;
        const char *path;
        int pcount = 0;
        if (!benc_isdct(fdct))
            return 0;
        if (benc_dget_int(fdct, "length") <= 0)
            return 0;
        if ((plst = benc_dget_lst(fdct, "path")) == NULL)
            return 0;
        path = benc_first(plst);
        while (path != NULL) {
            size_t plen;
            const char *pstr = benc_mem(path, &plen, &path);
            if (pstr == NULL || !mi_test_path(pstr, plen))
                return 0;
            pcount++;
        }
        if (pcount == 0)
            return 0;
        fcount++;
        fdct = benc_next(fdct);
    }
    return fcount > 0 ? 1 : 0;
}

static int
mi_test_announce_list(const char *alst)
{
    int lstcount = 0;
    const char *t = benc_first(alst);
    while (t != NULL && benc_islst(t)) {
        int strcount = 0;
        const char *s = benc_first(t);
        while (s != NULL && benc_isstr(s)) {
            strcount++;
            s = benc_next(s);
        }
        if (strcount == 0)
            return 0;
        lstcount++;
        t = benc_next(t);
    }
    return lstcount > 0 ? 1 : 0;
}

int
mi_test(const char *p, size_t size)
{
    const char *info;
    const char *alst;
    const char *pieces;
    const char *files;
    const char *fdct;
    const char *name;
    size_t slen, npieces;
    off_t length = 0, piece_length;

    if (benc_validate(p, size) != 0 || !benc_isdct(p))
        return 0;

    if ((alst = benc_dget_any(p, "announce-list")) != NULL) {
        if (!benc_islst(alst))
            return 0;
        if (!mi_test_announce_list(alst))
            return 0;
    } else if (benc_dget_mem(p, "announce", NULL) == NULL)
        return 0;

    if ((info = benc_dget_dct(p, "info")) == NULL)
        return 0;
    if ((name = benc_dget_mem(info, "name", &slen)) != NULL)
        if (!mi_test_path(name, slen))
            return 0;
    if ((piece_length = benc_dget_int(info, "piece length")) <= 0)
        return 0;
    if ((pieces = benc_dget_mem(info, "pieces", &slen)) == NULL ||
            slen % 20 != 0)
        return 0;
    npieces = slen / 20;
    if ((length = benc_dget_int(info, "length")) != 0) {
        if (length < 0 || benc_dget_any(info, "files") != NULL)
            return 0;
    } else {
        if ((files = benc_dget_lst(info, "files")) == NULL)
            return 0;
        if (!mi_test_files(files))
            return 0;
        fdct = benc_first(files);
        while (fdct != NULL) {
            length += benc_dget_int(fdct, "length");
            fdct = benc_next(fdct);
        }
    }
    if (length < (npieces - 1) * piece_length ||
            length > npieces * piece_length)
        return 0;
    return 1;
}

char *
mi_load(const char *path, size_t *size)
{
    void *res = NULL;
    size_t mi_size = (1 << 21);

    if ((errno = read_whole_file(&res, &mi_size, path)) != 0)
        return NULL;
    if (!mi_test(res, mi_size)) {
        free(res);
        errno = EINVAL;
        return NULL;
    }
    if (size != NULL)
        *size = mi_size;
    return res;
}
