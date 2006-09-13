#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "btpd.h"
#include "tracker_req.h"
#include "stream.h"

static unsigned m_ntorrents;
static struct torrent_tq m_torrents = BTPDQ_HEAD_INITIALIZER(m_torrents);

const struct torrent_tq *
torrent_get_all(void)
{
    return &m_torrents;
}

unsigned
torrent_count(void)
{
    return m_ntorrents;
}

struct torrent *
torrent_by_num(unsigned num)
{
    struct tlib *tl = tlib_by_num(num);
    return tl != NULL ? tl->tp : NULL;
}

struct torrent *
torrent_by_hash(const uint8_t *hash)
{
    struct tlib *tl = tlib_by_hash(hash);
    return tl != NULL ? tl->tp : NULL;
}

const char *
torrent_name(struct torrent *tp)
{
    return tp->tl->name;
}

off_t
torrent_piece_size(struct torrent *tp, uint32_t index)
{
    if (index < tp->npieces - 1)
        return tp->piece_length;
    else {
        off_t allbutlast = tp->piece_length * (tp->npieces - 1);
        return tp->total_length - allbutlast;
    }
}

uint32_t
torrent_piece_blocks(struct torrent *tp, uint32_t piece)
{
    return ceil(torrent_piece_size(tp, piece) / (double)PIECE_BLOCKLEN);
}

uint32_t
torrent_block_size(struct torrent *tp, uint32_t piece, uint32_t nblocks,
    uint32_t block)
{
    if (block < nblocks - 1)
        return PIECE_BLOCKLEN;
    else {
        uint32_t allbutlast = PIECE_BLOCKLEN * (nblocks - 1);
        return torrent_piece_size(tp, piece) - allbutlast;
    }
}

enum ipc_err
torrent_start(struct tlib *tl)
{
    struct stat sb;
    struct torrent *tp;
    char *mi;
    char relpath[RELPATH_SIZE];
    char file[PATH_MAX];

    if (tl->dir == NULL)
        return IPC_EBADTENT;

    if (mkdir(tl->dir, 0777) != 0 && errno != EEXIST) {
        btpd_log(BTPD_L_ERROR, "torrent '%s': "
            "failed to create content dir '%s' (%s).\n",
            tl->name, tl->dir, strerror(errno));
        return IPC_ECREATECDIR;
    } else if (stat(tl->dir, &sb) == -1 ||
            ((sb.st_mode & S_IFMT) != S_IFDIR)) {
        btpd_log(BTPD_L_ERROR,
            "torrent '%s': content dir '%s' is either not a directory or"
            " cannot be accessed.\n", tl->name, tl->dir);
        return IPC_EBADCDIR;
    }

    bin2hex(tl->hash, relpath, 20);
    snprintf(file, PATH_MAX, "torrents/%s/torrent", relpath);
    if ((mi = mi_load(file, NULL)) == NULL) {
        btpd_log(BTPD_L_ERROR,
            "torrent '%s': failed to load metainfo (%s).\n",
            tl->name, strerror(errno));
        return IPC_EBADTENT;
    }

    tp = btpd_calloc(1, sizeof(*tp));
    tp->tl = tl;
    bcopy(relpath, tp->relpath, RELPATH_SIZE);
    tp->files = mi_files(mi);
    tp->nfiles = mi_nfiles(mi);
    if (tp->files == NULL)
        btpd_err("out of memory.\n");
    tp->total_length = mi_total_length(mi);
    tp->piece_length = mi_piece_length(mi);
    tp->npieces = mi_npieces(mi);
    tp->pieces_off =
        benc_dget_mem(benc_dget_dct(mi, "info"), "pieces", NULL) - mi;

    btpd_log(BTPD_L_BTPD, "Starting torrent '%s'.\n", torrent_name(tp));
    if (tr_create(tp, mi) == 0) {
        net_create(tp);
        cm_create(tp);
        BTPDQ_INSERT_TAIL(&m_torrents, tp, entry);
        m_ntorrents++;
        cm_start(tp);
        tl->tp = tp;
        free(mi);
        return IPC_OK;
    } else {
        mi_free_files(tp->nfiles, tp->files);
        free(tp);
        free(mi);
        return IPC_EBADTRACKER;
    }
}

static void
torrent_kill(struct torrent *tp)
{
    btpd_log(BTPD_L_BTPD, "Stopped torrent '%s'.\n", torrent_name(tp));
    assert(m_ntorrents > 0);
    assert(!(tr_active(tp) || net_active(tp) || cm_active(tp)));
    m_ntorrents--;
    BTPDQ_REMOVE(&m_torrents, tp, entry);
    tr_kill(tp);
    net_kill(tp);
    cm_kill(tp);
    tp->tl->tp = NULL;
    mi_free_files(tp->nfiles, tp->files);
    free(tp);
    if (m_ntorrents == 0)
        btpd_on_no_torrents();
}

void
torrent_stop(struct torrent *tp)
{
    int tra, cma;
    switch (tp->state) {
    case T_STARTING:
    case T_ACTIVE:
        tp->state = T_STOPPING;
        if (net_active(tp))
            net_stop(tp);
        tra = tr_active(tp);
        cma = cm_active(tp);
        if (tra)
            tr_stop(tp);
        if (cma)
            cm_stop(tp);
        if (!(tra || cma))
            torrent_kill(tp);
        break;
    case T_STOPPING:
        if (tr_active(tp))
            tr_stop(tp);
        break;
    }
}

void
torrent_on_cm_started(struct torrent *tp)
{
    tp->state = T_ACTIVE;
    net_start(tp);
    tr_start(tp);
}

void
torrent_on_cm_stopped(struct torrent *tp)
{
    assert(tp->state == T_STOPPING);
    if (!tr_active(tp))
        torrent_kill(tp);
}

void
torrent_on_tr_stopped(struct torrent *tp)
{
    assert(tp->state == T_STOPPING);
    if (!cm_active(tp))
        torrent_kill(tp);
}
