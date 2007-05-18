#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "btpd.h"
#include "stream.h"

struct content {
    enum { CM_INACTIVE, CM_STARTING, CM_ACTIVE } state;

    int error;

    uint32_t npieces_got;

    off_t ncontent_bytes;

    size_t bppbf; // bytes per piece block field

    uint8_t *piece_field;
    uint8_t *block_field;
    uint8_t *pos_field;

    struct bt_stream *rds;
    struct bt_stream *wrs;

    struct resume_data *resd;
};

#define ZEROBUFLEN (1 << 14)

static const uint8_t m_zerobuf[ZEROBUFLEN];

static int
fd_cb_rd(const char *path, int *fd, void *arg)
{
    struct torrent *tp = arg;
    return vopen(fd, O_RDONLY, "%s/%s", tp->tl->dir, path);
}

static int
fd_cb_wr(const char *path, int *fd, void *arg)
{
    struct torrent *tp = arg;
    return vopen(fd, O_RDWR, "%s/%s", tp->tl->dir, path);
}

struct start_test_data {
    struct torrent *tp;
    struct file_time_size *fts;
    uint32_t start;
    BTPDQ_ENTRY(start_test_data) entry;
};

BTPDQ_HEAD(std_tq, start_test_data);

static struct std_tq m_startq = BTPDQ_HEAD_INITIALIZER(m_startq);

static struct event m_workev;

#define READBUFLEN (1 << 14)

static int
test_hash(struct torrent *tp, uint8_t *hash, uint32_t piece)
{
    char piece_hash[SHA_DIGEST_LENGTH];
    tlib_read_hash(tp->tl, tp->pieces_off, piece, piece_hash);
    return bcmp(hash, piece_hash, SHA_DIGEST_LENGTH);
}

static int
test_piece(struct torrent *tp, uint32_t piece, int *ok)
{
    int err;
    uint8_t hash[SHA_DIGEST_LENGTH];
    if ((err = bts_sha(tp->cm->rds, piece * tp->piece_length,
             torrent_piece_size(tp, piece), hash)) != 0) {
        btpd_log(BTPD_L_ERROR, "io error on '%s' (%s).\n",
            bts_filename(tp->cm->rds), strerror(err));
        return err;;
    }
    *ok = test_hash(tp, hash, piece) == 0;
    return 0;
}

static int test_hash(struct torrent *tp, uint8_t *hash, uint32_t piece);

static void startup_test_run(void);

void
worker_cb(int fd, short type, void *arg)
{
    startup_test_run();
}

void
cm_kill(struct torrent *tp)
{
    struct content *cm = tp->cm;
    tlib_close_resume(cm->resd);
    free(cm->pos_field);
    free(cm);
    tp->cm = NULL;
}

static int stat_and_adjust(struct torrent *tp, struct file_time_size ret[]);

void
cm_save(struct torrent *tp)
{
    struct file_time_size fts[tp->nfiles];
    stat_and_adjust(tp, fts);
    for (int i = 0; i < tp->nfiles; i++)
        resume_set_fts(tp->cm->resd, i, fts + i);
}

static void
cm_on_error(struct torrent *tp)
{
    if (!tp->cm->error) {
        tp->cm->error = 1;
        cm_stop(tp);
    }
}

static void
cm_write_done(struct torrent *tp)
{
    int err;
    struct content *cm = tp->cm;

    err = bts_close(cm->wrs);
    cm->wrs = NULL;
    if (err && !cm->error) {
        btpd_log(BTPD_L_ERROR, "error closing write stream for '%s' (%s).\n",
            torrent_name(tp), strerror(err));
        cm_on_error(tp);
    }
    if (!cm->error)
        cm_save(tp);
}

void
cm_stop(struct torrent *tp)
{
    struct content *cm = tp->cm;

    if (cm->state != CM_STARTING && cm->state != CM_ACTIVE)
        return;

    if (cm->state == CM_STARTING) {
        struct start_test_data *std;
        BTPDQ_FOREACH(std, &m_startq, entry)
            if (std->tp == tp) {
                BTPDQ_REMOVE(&m_startq, std, entry);
                free(std->fts);
                free(std);
                break;
            }
    }

    if (cm->rds != NULL)
        bts_close(cm->rds);
    if (cm->wrs != NULL)
        cm_write_done(tp);

    cm->state = CM_INACTIVE;
}

int
cm_active(struct torrent *tp)
{
    struct content *cm = tp->cm;
    return cm->state != CM_INACTIVE;
}

int
cm_error(struct torrent *tp)
{
    return tp->cm->error;
}

int
cm_started(struct torrent *tp)
{
    struct content *cm = tp->cm;
    return cm->state == CM_ACTIVE;
}

void
cm_create(struct torrent *tp, const char *mi)
{
    size_t pfield_size = ceil(tp->npieces / 8.0);
    struct content *cm = btpd_calloc(1, sizeof(*cm));
    cm->bppbf = ceil((double)tp->piece_length / (1 << 17));
    cm->pos_field = btpd_calloc(pfield_size, 1);
    cm->resd = tlib_open_resume(tp->tl, tp->nfiles, pfield_size,
        cm->bppbf * tp->npieces);
    cm->piece_field = resume_piece_field(cm->resd);
    cm->block_field = resume_block_field(cm->resd);

    tp->cm = cm;
}

int
cm_get_bytes(struct torrent *tp, uint32_t piece, uint32_t begin, size_t len,
    uint8_t **buf)
{
    if (tp->cm->error)
        return EIO;

    *buf = btpd_malloc(len);
    int err =
        bts_get(tp->cm->rds, piece * tp->piece_length + begin, *buf, len);
    if (err != 0) {
        btpd_log(BTPD_L_ERROR, "io error on '%s' (%s).\n",
            bts_filename(tp->cm->rds), strerror(err));
        cm_on_error(tp);
    }
    return err;
}

void
cm_prealloc(struct torrent *tp, uint32_t piece)
{
    struct content *cm = tp->cm;

    if (cm_alloc_size <= 0)
        set_bit(cm->pos_field, piece);
}

void
cm_test_piece(struct torrent *tp, uint32_t piece)
{
    int ok;
    struct content *cm = tp->cm;
    if ((errno = test_piece(tp, piece, &ok)) != 0)
        cm_on_error(tp);
    else if (ok) {
        assert(cm->npieces_got < tp->npieces);
        cm->npieces_got++;
        set_bit(cm->piece_field, piece);
        if (net_active(tp))
            dl_on_ok_piece(tp->net,piece);
        if (cm_full(tp))
            cm_write_done(tp);
    } else {
        cm->ncontent_bytes -= torrent_piece_size(tp,piece);
        bzero(cm->block_field + piece * cm->bppbf, cm->bppbf);
        if (net_active(tp))
            dl_on_bad_piece(tp->net, piece);
    }
}

int
cm_put_bytes(struct torrent *tp, uint32_t piece, uint32_t begin,
    const uint8_t *buf, size_t len)
{
    int err;
    struct content *cm = tp->cm;

    if (cm->error)
        return EIO;

    uint8_t *bf = cm->block_field + piece * cm->bppbf;
    assert(!has_bit(bf, begin / PIECE_BLOCKLEN));
    assert(!has_bit(cm->piece_field, piece));

    if (!has_bit(cm->pos_field, piece)) {
        unsigned npieces = ceil((double)cm_alloc_size / tp->piece_length);
        uint32_t start = piece - piece % npieces;
        uint32_t end = min(start + npieces, tp->npieces);

        while (start < end) {
            if (!has_bit(cm->pos_field, start)) {
                assert(!has_bit(cm->piece_field, start));
                off_t len = torrent_piece_size(tp, start);
                off_t off = tp->piece_length * start;
                while (len > 0) {
                    size_t wlen = min(ZEROBUFLEN, len);
                    if ((err = bts_put(cm->wrs, off, m_zerobuf, wlen)) != 0) {
                        btpd_log(BTPD_L_ERROR, "io error on '%s' (%s).\n",
                            bts_filename(cm->wrs), strerror(errno));
                        cm_on_error(tp);
                        return err;
                    }

                    len -= wlen;
                    off += wlen;
                }
                set_bit(cm->pos_field, start);
            }
            start++;
        }
    }
    err = bts_put(cm->wrs, piece * tp->piece_length + begin, buf, len);
    if (err != 0) {
        btpd_log(BTPD_L_ERROR, "io error on '%s' (%s)\n",
            bts_filename(cm->wrs), strerror(err));
        cm_on_error(tp);
        return err;
    }

    cm->ncontent_bytes += len;
    set_bit(bf, begin / PIECE_BLOCKLEN);

    return 0;
}

int
cm_full(struct torrent *tp)
{
    return tp->cm->npieces_got == tp->npieces;
}

off_t
cm_content(struct torrent *tp)
{
    return tp->cm->ncontent_bytes;
}

uint32_t
cm_pieces(struct torrent *tp)
{
    return tp->cm->npieces_got;
}

uint8_t *
cm_get_piece_field(struct torrent *tp)
{
    return tp->cm->piece_field;
}

uint8_t *
cm_get_block_field(struct torrent *tp, uint32_t piece)
{
    return tp->cm->block_field + piece * tp->cm->bppbf;
}

int
cm_has_piece(struct torrent *tp, uint32_t piece)
{
    return has_bit(tp->cm->piece_field, piece);
}

int
stat_and_adjust(struct torrent *tp, struct file_time_size ret[])
{
    int fd;
    char path[PATH_MAX];
    struct stat sb;
    for (int i = 0; i < tp->nfiles; i++) {
        snprintf(path, PATH_MAX, "%s/%s", tp->tl->dir, tp->files[i].path);
again:
        if (stat(path, &sb) == -1) {
            if (errno == ENOENT) {
                errno = vopen(&fd, O_CREAT|O_RDWR, "%s", path);
                if (errno != 0 || close(fd) != 0) {
                    btpd_log(BTPD_L_ERROR, "failed to create '%s' (%s).\n",
                        path, strerror(errno));
                    return errno;
                }
                goto again;
            } else {
                btpd_log(BTPD_L_ERROR, "failed to stat '%s' (%s).\n",
                    path, strerror(errno));
                return errno;
            }
        } else if (sb.st_size > tp->files[i].length) {
            if (truncate(path, tp->files[i].length) != 0) {
                btpd_log(BTPD_L_ERROR, "failed to truncate '%s' (%s).\n",
                    path, strerror(errno));
                return errno;
            }
            goto again;
        } else {
            ret[i].mtime = sb.st_mtime;
            ret[i].size = sb.st_size;
        }
    }
    return 0;
}

void
startup_test_end(struct torrent *tp, int unclean)
{
    struct content *cm = tp->cm;

    bzero(cm->pos_field, ceil(tp->npieces / 8.0));
    for (uint32_t piece = 0; piece < tp->npieces; piece++) {
        if (cm_has_piece(tp, piece)) {
            cm->ncontent_bytes += torrent_piece_size(tp, piece);
            cm->npieces_got++;
            set_bit(cm->pos_field, piece);
            continue;
        }
        uint8_t *bf = cm->block_field + cm->bppbf * piece;
        uint32_t nblocks = torrent_piece_blocks(tp, piece);
        uint32_t nblocks_got = 0;
        for (uint32_t i = 0; i < nblocks; i++) {
            if (has_bit(bf, i)) {
                nblocks_got++;
                cm->ncontent_bytes +=
                    torrent_block_size(tp, piece, nblocks, i);
            }
        }
        if (nblocks_got == nblocks) {
            bzero(bf, cm->bppbf);
            cm->ncontent_bytes -= torrent_piece_size(tp, piece);
        } else if (nblocks_got > 0)
            set_bit(cm->pos_field, piece);
    }
    if (unclean) {
        struct start_test_data *std = BTPDQ_FIRST(&m_startq);
        BTPDQ_REMOVE(&m_startq, std, entry);
        for (int i = 0; i < tp->nfiles; i++)
            resume_set_fts(cm->resd, i, std->fts + i);
        free(std->fts);
        free(std);
    }
    if (!cm_full(tp)) {
        int err;
        if ((err = bts_open(&cm->wrs, tp->nfiles, tp->files,
                 fd_cb_wr, tp)) != 0) {
            btpd_log(BTPD_L_ERROR,
                "failed to open write stream for '%s' (%s).\n",
                torrent_name(tp), strerror(err));
            cm_on_error(tp);
            return;
        }
    }
    cm->state = CM_ACTIVE;
}

void
startup_test_run(void)
{
    int ok;
    struct torrent *tp;
    struct content *cm;
    struct start_test_data * std = BTPDQ_FIRST(&m_startq);
    uint32_t this;
    if (std == NULL)
        return;
    tp = std->tp;
    cm = tp->cm;
    if (test_piece(std->tp, std->start, &ok) != 0) {
        cm_on_error(std->tp);
        return;
    }
    if (ok)
        set_bit(cm->piece_field, std->start);
    else
        clear_bit(cm->piece_field, std->start);
    this = std->start;
    do
        std->start++;
    while (std->start < tp->npieces && !has_bit(cm->pos_field, std->start));
    if (std->start >= tp->npieces)
        startup_test_end(tp, 1);
    if (!BTPDQ_EMPTY(&m_startq))
        event_add(&m_workev, (& (struct timeval) { 0, 0 }));
}

void
startup_test_begin(struct torrent *tp, struct file_time_size *fts)
{
    uint32_t piece = 0;
    struct content *cm = tp->cm;
    while (piece < tp->npieces && !has_bit(cm->pos_field, piece))
        piece++;
    if (piece < tp->npieces) {
        struct start_test_data *std = btpd_calloc(1, sizeof(*std));
        std->tp = tp;
        std->start = piece;
        std->fts = fts;
        BTPDQ_INSERT_TAIL(&m_startq, std, entry);
        if (std == BTPDQ_FIRST(&m_startq))
            event_add(&m_workev, (& (struct timeval) { 0, 0 }));
    } else {
        free(fts);
        startup_test_end(tp, 0);
    }
}

void
cm_start(struct torrent *tp, int force_test)
{
    int err, run_test = force_test;
    struct file_time_size *fts;
    struct content *cm = tp->cm;

    cm->state = CM_STARTING;

    if ((errno =
            bts_open(&cm->rds, tp->nfiles, tp->files, fd_cb_rd, tp)) != 0) {
        btpd_log(BTPD_L_ERROR, "failed to open stream for '%s' (%s).\n",
            torrent_name(tp), strerror(errno));
        cm_on_error(tp);
        return;
    }

    fts = btpd_calloc(tp->nfiles, sizeof(*fts));

    if ((err = stat_and_adjust(tp, fts)) != 0) {
        free(fts);
        cm_on_error(tp);
        return;
    }

    for (int i = 0; i < tp->nfiles; i++) {
        struct file_time_size rfts;
        resume_get_fts(cm->resd, i, &rfts);
        if ((fts[i].mtime != rfts.mtime || fts[i].size != rfts.size)) {
            run_test = 1;
            break;
        }
    }
    if (run_test) {
        memset(cm->pos_field, 0xff, ceil(tp->npieces / 8.0));
        off_t off = 0;
        for (int i = 0; i < tp->nfiles; i++) {
            if (fts[i].size != tp->files[i].length) {
                uint32_t start, end;
                end = (off + tp->files[i].length - 1)
                    / tp->piece_length;
                start = (off + fts[i].size) / tp->piece_length;
                while (start <= end) {
                    clear_bit(cm->pos_field, start);
                    clear_bit(cm->piece_field, start);
                    bzero(cm->block_field + start * cm->bppbf, cm->bppbf);
                    start++;
                }
            }
            off += tp->files[i].length;
        }
    }

    startup_test_begin(tp, fts);
}

void
cm_init(void)
{
    evtimer_set(&m_workev, worker_cb, NULL);
}
