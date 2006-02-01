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

off_t
torrent_piece_size(struct torrent *tp, uint32_t index)
{
    if (index < tp->meta.npieces - 1)
        return tp->meta.piece_length;
    else {
        off_t allbutlast = tp->meta.piece_length * (tp->meta.npieces - 1);
        return tp->meta.total_length - allbutlast;
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

void
torrent_activate(struct torrent *tp)
{
    if (tp->state == T_INACTIVE) {
        tp->state = T_STARTING;
        cm_start(tp);
        btpd_tp_activated(tp);
    }
}

void
torrent_deactivate(struct torrent *tp)
{
    switch (tp->state) {
    case T_INACTIVE:
        break;
    case T_STARTING:
    case T_ACTIVE:
        tp->state = T_STOPPING;
        if (tp->tr != NULL)
            tr_stop(tp);
        if (tp->net != NULL)
            net_del_torrent(tp);
        if (tp->cm != NULL)
            cm_stop(tp);
        break;
    case T_STOPPING:
        if (tp->tr != NULL)
            tr_destroy(tp);
        break;
    default:
        abort();
    }
}

int
torrent_load(struct torrent **res, const char *path)
{
    struct metainfo *mi;
    int error;
    char file[PATH_MAX];
    snprintf(file, PATH_MAX, "library/%s/torrent", path);

    if ((error = load_metainfo(file, -1, 0, &mi)) != 0) {
        btpd_log(BTPD_L_ERROR, "Couldn't load metainfo file %s: %s.\n",
            file, strerror(error));
        return error;
    }

    if (btpd_get_torrent(mi->info_hash) != NULL) {
        btpd_log(BTPD_L_BTPD,
            "%s has same hash as an already loaded torrent.\n", path);
        error = EEXIST;
    }

    if (error == 0) {
        *res = btpd_calloc(1, sizeof(**res));
        (*res)->relpath = strdup(path);
        (*res)->meta = *mi;
        free(mi);
    } else {
        clear_metainfo(mi);
        free(mi);
    }

    return error;
}

void
torrent_on_cm_started(struct torrent *tp)
{
    net_add_torrent(tp);
    tr_start(tp);
    tp->state = T_ACTIVE;
}

void
torrent_on_cm_stopped(struct torrent *tp)
{
    assert(tp->state == T_STOPPING);
    if (tp->tr == NULL) {
        tp->state = T_INACTIVE;
        btpd_tp_deactivated(tp);
    }
}

void
torrent_on_tr_stopped(struct torrent *tp)
{
    assert(tp->state == T_STOPPING);
    if (tp->cm == NULL) {
        tp->state = T_INACTIVE;
        btpd_tp_deactivated(tp);
    }
}
