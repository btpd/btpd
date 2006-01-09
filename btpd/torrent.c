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
torrent_block_size(struct piece *pc, uint32_t index)
{
    if (index < pc->nblocks - 1)
        return PIECE_BLOCKLEN;
    else {
        uint32_t allbutlast = PIECE_BLOCKLEN * (pc->nblocks - 1);
        return torrent_piece_size(pc->n->tp, pc->index) - allbutlast;
    }
}

void
torrent_activate(struct torrent *tp)
{
    assert(tp->state == T_INACTIVE);
    tp->state = T_STARTING;
    cm_start(tp);
}

void
torrent_deactivate(struct torrent *tp)
{

}

int
torrent_create(struct torrent **res, const char *path)
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

void torrent_cm_cb(struct torrent *tp, enum cm_state state)
{
    switch (state) {
    case CM_STARTED:
        net_add_torrent(tp);
        tr_start(tp);
        break;
    case CM_STOPPED:
        break;
    case CM_ERROR:
        break;
    }
}
