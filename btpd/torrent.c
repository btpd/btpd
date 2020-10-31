#include "btpd.h"

#define SAVE_INTERVAL 300

static unsigned m_nghosts;
static unsigned m_ntorrents;
static struct torrent_tq m_torrents = BTPDQ_HEAD_INITIALIZER(m_torrents);

static unsigned m_tsave;
static struct torrent *m_savetp;

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

unsigned
torrent_ghosts(void)
{
    return m_nghosts;
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

static void
torrent_kill(struct torrent *tp)
{
    assert(m_ntorrents > 0);
    assert(!(net_active(tp) || cm_active(tp)));
    if (tp->state == T_GHOST)
        m_nghosts--;
    m_ntorrents--;
    BTPDQ_REMOVE(&m_torrents, tp, entry);
    if (tp->delete)
        tlib_kill(tp->tl);
    else
        tp->tl->tp = NULL;
    tr_kill(tp);
    net_kill(tp);
    cm_kill(tp);
    mi_free_files(tp->nfiles, tp->files);
    if (m_savetp == tp)
        if ((m_savetp = BTPDQ_NEXT(tp, entry)) == NULL)
            m_savetp = BTPDQ_FIRST(&m_torrents);
    free(tp);
}

enum ipc_err
torrent_start(struct tlib *tl)
{
    struct torrent *tp;
    char *mi;

    if (tl->tp != NULL) {
        assert(torrent_startable(tl));
        torrent_kill(tl->tp);
        tl->tp = NULL;
    }

    if (tl->dir == NULL)
        return IPC_EBADTENT;

    if (tlib_load_mi(tl, &mi) != 0)
        return IPC_EBADTENT;

    tp = btpd_calloc(1, sizeof(*tp));
    tp->tl = tl;
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
    tr_create(tp, mi);
    tl->tp = tp;
    net_create(tp);
    cm_create(tp, mi);
    BTPDQ_INSERT_TAIL(&m_torrents, tp, entry);
    m_ntorrents++;
    cm_start(tp, 0);
    free(mi);
    if (m_ntorrents == 1) {
        m_tsave = btpd_seconds + SAVE_INTERVAL;
        m_savetp = tp;
    }
    return IPC_OK;
}

static
void become_ghost(struct torrent *tp)
{
    btpd_log(BTPD_L_BTPD, "Stopped torrent '%s'.\n", torrent_name(tp));
    tp->state = T_GHOST;
    if (tp->delete)
        tlib_del(tp->tl);
    else
        tlib_update_info(tp->tl, 0);
    m_nghosts++;
}

void
torrent_stop(struct torrent *tp, int delete)
{
    if (delete)
        tp->delete = 1;
    switch (tp->state) {
    case T_LEECH:
    case T_SEED:
    case T_STARTING:
        tp->state = T_STOPPING;
        if (net_active(tp))
            net_stop(tp);
        if (tr_active(tp))
            tr_stop(tp);
        if (cm_active(tp))
            cm_stop(tp);
        if (!cm_active(tp)) {
            become_ghost(tp);
            if (!tr_active(tp))
                torrent_kill(tp);
        }
        break;
    default:
        break;
    }
}

void
torrent_on_tick(struct torrent *tp)
{
    if (tp->state != T_STOPPING && cm_error(tp))
        torrent_stop(tp, 0);
    switch (tp->state) {
    case T_STARTING:
        if (cm_started(tp)) {
            if (cm_full(tp))
                tp->state = T_SEED;
            else
                tp->state = T_LEECH;
            net_start(tp);
            tr_start(tp);
        }
        break;
    case T_LEECH:
        if (cm_full(tp)) {
            struct peer *p, *next;
            tp->state = T_SEED;
            btpd_log(BTPD_L_BTPD, "Finished downloading '%s'.\n",
                torrent_name(tp));
            tr_complete(tp);
            BTPDQ_FOREACH_MUTABLE(p, &tp->net->peers, p_entry, next) {
                assert(p->nwant == 0);
                if (peer_full(p))
                    peer_kill(p);
            }
        }
        break;
    case T_STOPPING:
        if (cm_active(tp))
            break;
        become_ghost(tp);
    case T_GHOST:
        if (!tr_active(tp))
            torrent_kill(tp);
        break;
    default:
        break;
    }
}

void
torrent_on_tick_all(void)
{
    struct torrent *tp, *next;
    BTPDQ_FOREACH_MUTABLE(tp, &m_torrents, entry, next)
        torrent_on_tick(tp);

    if (m_savetp != NULL && m_tsave <= btpd_seconds) {
        if (m_savetp->state == T_LEECH || m_savetp->state == T_SEED) {
            tlib_update_info(m_savetp->tl, 1);
            if ((m_savetp = BTPDQ_NEXT(m_savetp, entry)) == NULL)
                m_savetp = BTPDQ_FIRST(&m_torrents);
            if (m_ntorrents > 0)
                m_tsave = btpd_seconds +
                    max(m_ntorrents, SAVE_INTERVAL) / m_ntorrents;
        }
    }
}

int
torrent_active(struct tlib *tl)
{
    return tl->tp != NULL && tl->tp->state != T_GHOST;
}

int
torrent_startable(struct tlib *tl)
{
    return tl->tp == NULL || (tl->tp->state == T_GHOST && !tl->tp->delete);
}

int
torrent_haunting(struct tlib *tl)
{
    return tl->tp != NULL && tl->tp->delete && tl->tp->state == T_GHOST;
}
