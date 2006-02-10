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
torrent_get(const uint8_t *hash)
{
    struct torrent *tp = BTPDQ_FIRST(&m_torrents);
    while (tp != NULL && bcmp(hash, tp->meta.info_hash, 20) != 0)
        tp = BTPDQ_NEXT(tp, entry);
    return tp;
}

const char *
torrent_name(struct torrent *tp)
{
    return tp->meta.name;
}

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

static void
torrent_relpath(const uint8_t *hash, char *buf)
{
    for (int i = 0; i < 20; i++)
        snprintf(buf + i * 2, 3, "%.2x", hash[i]);
}

int
torrent_set_links(const uint8_t *hash, const char *torrent,
    const char *content)
{
    char relpath[RELPATH_SIZE];
    char file[PATH_MAX];
    torrent_relpath(hash, relpath);
    snprintf(file, PATH_MAX, "torrents/%s", relpath);
    if (mkdir(file, 0777) == -1 && errno != EEXIST)
        return errno;
    snprintf(file, PATH_MAX, "torrents/%s/torrent", relpath);
    if (unlink(file) == -1 && errno != ENOENT)
        return errno;
    if (symlink(torrent, file) == -1)
        return errno;
    snprintf(file, PATH_MAX, "torrents/%s/content", relpath);
    if (unlink(file) == -1 && errno != ENOENT)
        return errno;
    if (symlink(content, file) == -1)
        return errno;
    return 0;
}

int
torrent_start(const uint8_t *hash)
{
    struct torrent *tp;
    struct metainfo *mi;
    int error;
    char relpath[RELPATH_SIZE];
    char file[PATH_MAX];

    torrent_relpath(hash, relpath);
    snprintf(file, PATH_MAX, "torrents/%s/torrent", relpath);

    if ((error = load_metainfo(file, -1, 0, &mi)) != 0) {
        btpd_log(BTPD_L_ERROR, "Couldn't load torrent file %s: %s.\n",
            file, strerror(error));
        return error;
    }

    tp = btpd_calloc(1, sizeof(*tp));
    bcopy(relpath, tp->relpath, RELPATH_SIZE);
    tp->meta = *mi;
    free(mi);

    btpd_log(BTPD_L_BTPD, "Starting torrent '%s'.\n", torrent_name(tp));
    if ((error = tr_create(tp)) == 0) {
        net_create(tp);
        cm_create(tp);
        BTPDQ_INSERT_TAIL(&m_torrents, tp, entry);
        m_ntorrents++;
        cm_start(tp);
    } else {
        clear_metainfo(&tp->meta);
        free(tp);
    }
    return error;
}

static void
torrent_kill(struct torrent *tp)
{
    btpd_log(BTPD_L_BTPD, "Removed torrent '%s'.\n", torrent_name(tp));
    assert(m_ntorrents > 0);
    assert(!(tr_active(tp) || net_active(tp) || cm_active(tp)));
    m_ntorrents--;
    BTPDQ_REMOVE(&m_torrents, tp, entry);
    clear_metainfo(&tp->meta);
    tr_kill(tp);
    net_kill(tp);
    cm_kill(tp);
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
