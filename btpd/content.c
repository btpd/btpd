#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "btpd.h"
#include "stream.h"

struct cm_write_data {
    uint32_t begin;
    uint8_t *buf;
    size_t len;
    BTPDQ_ENTRY(cm_write_data) entry;
};

BTPDQ_HEAD(cm_write_data_tq, cm_write_data);

enum cm_op_type {
    CM_ALLOC,
    CM_SAVE,
    CM_START,
    CM_TEST,
    CM_WRITE
};

struct cm_op {
    struct torrent *tp;
    int error;
    int received;
    enum cm_op_type type;
    union {
        struct {
            uint32_t piece;
            uint32_t pos;
        } alloc;
        struct {
            volatile sig_atomic_t cancel;
        } start;
        struct {
            uint32_t piece;
            uint32_t pos;
            int ok;
        } test;
        struct {
            uint32_t piece;
            uint32_t pos;
            struct cm_write_data_tq q;
        } write;
    } u;

    BTPDQ_ENTRY(cm_op) cm_entry;
    BTPDQ_ENTRY(cm_op) td_entry;
};

BTPDQ_HEAD(cm_op_tq, cm_op);

struct content {
    int active;

    uint32_t npieces_got;

    off_t ncontent_bytes;

    size_t bppbf; // bytes per piece block field

    uint8_t *piece_field;
    uint8_t *block_field;
    uint8_t *hold_field;
    uint8_t *pos_field;

    struct cm_op_tq todoq;

    struct bt_stream *rds;
    struct bt_stream *wrs;

    struct event save_timer;
};

#define ZEROBUFLEN (1 << 14)

struct cm_comm {
    struct cm_op_tq q;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

static struct cm_comm m_long_comm, m_short_comm;
static const uint8_t m_zerobuf[ZEROBUFLEN];

static int
fd_cb_rd(const char *path, int *fd, void *arg)
{
    struct torrent *tp = arg;
    return vopen(fd, O_RDONLY, "torrents/%s/content/%s", tp->relpath, path);
}

static int
fd_cb_wr(const char *path, int *fd, void *arg)
{
    struct torrent *tp = arg;
    return vopen(fd, O_RDWR|O_CREAT, "torrents/%s/content/%s", tp->relpath,
        path);
}

static void
cm_td_post_common(struct cm_comm *comm, struct cm_op *op)
{
    pthread_mutex_lock(&comm->lock);
    BTPDQ_INSERT_TAIL(&comm->q, op, td_entry);
    pthread_mutex_unlock(&comm->lock);
    pthread_cond_signal(&comm->cond);
}

static void
cm_td_post_long(struct cm_op *op)
{
    cm_td_post_common(&m_long_comm, op);
}

static void
cm_td_post_short(struct cm_op *op)
{
    cm_td_post_common(&m_short_comm, op);
}

static void
run_todo(struct content *cm)
{
    struct cm_op *op = BTPDQ_FIRST(&cm->todoq);

    if (op->type == CM_WRITE && BTPDQ_EMPTY(&op->u.write.q)) {
        BTPDQ_REMOVE(&cm->todoq, op, cm_entry);
        free(op);
        if (!BTPDQ_EMPTY(&cm->todoq))
            run_todo(cm);
        return;
    }

    if (op->type != CM_START)
        cm_td_post_short(op);
    else
        cm_td_post_long(op);
}

static void
add_todo(struct content *cm, struct cm_op *op)
{
    int was_empty = BTPDQ_EMPTY(&cm->todoq);
    BTPDQ_INSERT_TAIL(&cm->todoq, op, cm_entry);
    if (was_empty)
        run_todo(cm);
}

void
cm_kill(struct torrent *tp)
{
    struct content *cm = tp->cm;
    bts_close(cm->rds);
    free(cm->piece_field);
    free(cm->block_field);
    free(cm->hold_field);
    free(cm->pos_field);
    tp->cm = NULL;
}

void
cm_save(struct torrent *tp)
{
    struct content *cm = tp->cm;
    struct cm_op *op = btpd_calloc(1, sizeof(*op));
    op->tp = tp;
    op->type = CM_SAVE;
    add_todo(cm, op);
}

static void
cm_write_done(struct torrent *tp)
{
    int err;
    struct content *cm = tp->cm;

    if ((err = bts_close(cm->wrs)) != 0)
        btpd_err("Error closing write stream for %s (%s).\n", tp->relpath,
            strerror(err));
    cm->wrs = NULL;
    event_del(&cm->save_timer);
    cm_save(tp);
}

void
cm_stop(struct torrent *tp)
{
    struct content *cm = tp->cm;
    cm->active = 0;
    struct cm_op *op = BTPDQ_FIRST(&cm->todoq);
    if (op != NULL && op->type == CM_START) {
        pthread_mutex_lock(&m_long_comm.lock);
        if (op->received)
            op->u.start.cancel = 1;
        else {
            BTPDQ_REMOVE(&m_long_comm.q, op, td_entry);
            BTPDQ_REMOVE(&cm->todoq, op, cm_entry);
            free(op);
        }
        pthread_mutex_unlock(&m_long_comm.lock);
    } else if (!cm_full(tp))
        cm_write_done(tp);

    if (BTPDQ_EMPTY(&cm->todoq))
        torrent_on_cm_stopped(tp);
}

int
cm_active(struct torrent *tp)
{
    struct content *cm = tp->cm;
    return cm->active || !BTPDQ_EMPTY(&cm->todoq);
}

#define SAVE_INTERVAL (& (struct timeval) { 15, 0 })

static void
save_timer_cb(int fd, short type, void *arg)
{
    struct torrent *tp = arg;
    event_add(&tp->cm->save_timer, SAVE_INTERVAL);
    cm_save(tp);
}

static void
cm_td_cb(void *arg)
{
    int err;
    struct cm_op *op = arg;
    struct torrent *tp = op->tp;
    struct content *cm = tp->cm;

    if (op->error)
        btpd_err("IO error for %s.\n", tp->relpath);

    switch (op->type) {
    case CM_ALLOC:
        set_bit(cm->pos_field, op->u.alloc.pos);
        clear_bit(cm->hold_field, op->u.alloc.piece);
        break;
    case CM_START:
        if (cm->active) {
            assert(!op->u.start.cancel);
            if (!cm_full(tp)) {
                if ((err = bts_open(&cm->wrs, &tp->meta, fd_cb_wr, tp)) != 0)
                    btpd_err("Couldn't open write stream for %s (%s).\n",
                        tp->relpath, strerror(err));
                event_add(&cm->save_timer, SAVE_INTERVAL);
            }
            torrent_on_cm_started(tp);
        }
        break;
    case CM_TEST:
        if (op->u.test.ok) {
            assert(cm->npieces_got < tp->meta.npieces);
            cm->npieces_got++;
            set_bit(cm->piece_field, op->u.test.piece);
            if (net_active(tp))
                dl_on_ok_piece(op->tp->net, op->u.test.piece);
            if (cm_full(tp))
                cm_write_done(tp);
        } else {
            cm->ncontent_bytes -= torrent_piece_size(tp, op->u.test.piece);
            bzero(cm->block_field + op->u.test.piece * cm->bppbf, cm->bppbf);
            if (net_active(tp))
                dl_on_bad_piece(tp->net, op->u.test.piece);
        }
        break;
    case CM_SAVE:
    case CM_WRITE:
        break;
    }
    BTPDQ_REMOVE(&cm->todoq, op, cm_entry);
    free(op);
    if (!BTPDQ_EMPTY(&cm->todoq))
        run_todo(cm);
    else if (!cm->active)
        torrent_on_cm_stopped(tp);
}

void
cm_create(struct torrent *tp)
{
    size_t pfield_size = ceil(tp->meta.npieces / 8.0);
    struct content *cm = btpd_calloc(1, sizeof(*cm));
    cm->bppbf = ceil((double)tp->meta.piece_length / (1 << 17));
    cm->piece_field = btpd_calloc(pfield_size, 1);
    cm->hold_field = btpd_calloc(pfield_size, 1);
    cm->pos_field = btpd_calloc(pfield_size, 1);
    cm->block_field = btpd_calloc(tp->meta.npieces * cm->bppbf, 1);

    BTPDQ_INIT(&cm->todoq);
    evtimer_set(&cm->save_timer, save_timer_cb, tp);

    tp->cm = cm;
}

void
cm_start(struct torrent *tp)
{
    struct content *cm = tp->cm;

    if ((errno = bts_open(&cm->rds, &tp->meta, fd_cb_rd, tp)) != 0)
        btpd_err("Error opening stream (%s).\n", strerror(errno));

    cm->active = 1;
    struct cm_op *op = btpd_calloc(1, sizeof(*op));
    op->tp = tp;
    op->type = CM_START;
    add_todo(cm, op);
}

int
cm_get_bytes(struct torrent *tp, uint32_t piece, uint32_t begin, size_t len,
    uint8_t **buf)
{
    *buf = btpd_malloc(len);
    int err =
        bts_get(tp->cm->rds, piece * tp->meta.piece_length + begin, *buf, len);
    if (err != 0)
        btpd_err("Io error (%s)\n", strerror(err));
    return 0;
}

static void
cm_post_alloc(struct torrent *tp, uint32_t piece)
{
    struct content *cm = tp->cm;
    set_bit(cm->hold_field, piece);

    struct cm_op *op = btpd_calloc(1, sizeof(*op));
    op->tp = tp;
    op->type = CM_ALLOC;
    op->u.alloc.piece = piece;
    op->u.alloc.pos = piece;
    add_todo(cm, op);

    op = btpd_calloc(1, sizeof(*op));
    op->tp = tp;
    op->type = CM_WRITE;
    op->u.write.piece = piece;
    op->u.write.pos = piece;
    BTPDQ_INIT(&op->u.write.q);
    add_todo(cm, op);
}

void
cm_prealloc(struct torrent *tp, uint32_t piece)
{
    struct content *cm = tp->cm;

    if (cm_alloc_size == 0)
        set_bit(cm->pos_field, piece);
    else {
        unsigned npieces = ceil((double)cm_alloc_size / tp->meta.piece_length);
        uint32_t start = piece - piece % npieces;
        uint32_t end = min(start + npieces, tp->meta.npieces);

        while (start < end) {
            if ((!has_bit(cm->pos_field, start)
                    && !has_bit(cm->hold_field, start)))
                cm_post_alloc(tp, start);
            start++;
        }
    }
}

void
cm_test_piece(struct torrent *tp, uint32_t piece)
{
    struct content *cm = tp->cm;
    struct cm_op *op = btpd_calloc(1, sizeof(*op));
    op->tp = tp;
    op->type = CM_TEST;
    op->u.test.piece = piece;
    op->u.test.pos = piece;
    add_todo(cm, op);
}

int
cm_put_bytes(struct torrent *tp, uint32_t piece, uint32_t begin,
    const uint8_t *buf, size_t len)
{
    int err;
    struct content *cm = tp->cm;

    if (has_bit(cm->hold_field, piece)) {
        struct cm_write_data *d = btpd_calloc(1, sizeof(*d) + len);
        d->begin = begin;
        d->len = len;
        d->buf = (uint8_t *)(d + 1);
        bcopy(buf, d->buf, len);
        struct cm_op *op;
        BTPDQ_FOREACH(op, &cm->todoq, cm_entry)
            if (op->type == CM_WRITE && op->u.write.piece == piece)
                break;
        struct cm_write_data *it;
        BTPDQ_FOREACH(it, &op->u.write.q, entry)
            if (it->begin > begin) {
                BTPDQ_INSERT_BEFORE(it, d, entry);
                break;
            }
        if (it == NULL)
            BTPDQ_INSERT_TAIL(&op->u.write.q, d, entry);
    } else {
        err = bts_put(cm->wrs, piece * tp->meta.piece_length + begin, buf,
            len);
        if (err != 0)
            btpd_err("Io error (%s)\n", strerror(err));
    }

    cm->ncontent_bytes += len;
    uint8_t *bf = cm->block_field + piece * cm->bppbf;
    set_bit(bf, begin / PIECE_BLOCKLEN);

    return 0;
}

int
cm_full(struct torrent *tp)
{
    return tp->cm->npieces_got == tp->meta.npieces;
}

off_t
cm_get_size(struct torrent *tp)
{
    return tp->cm->ncontent_bytes;
}

uint32_t
cm_get_npieces(struct torrent *tp)
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

static int
test_hash(struct torrent *tp, uint8_t *hash, uint32_t piece)
{
    if (tp->meta.piece_hash != NULL)
        return bcmp(hash, tp->meta.piece_hash[piece], SHA_DIGEST_LENGTH);
    else {
        char piece_hash[SHA_DIGEST_LENGTH];
        int fd;
        int err;

        err = vopen(&fd, O_RDONLY, "torrents/%s/torrent", tp->relpath);
        if (err != 0)
            btpd_err("test_hash: %s\n", strerror(err));

        lseek(fd, tp->meta.pieces_off + piece * SHA_DIGEST_LENGTH, SEEK_SET);
        read(fd, piece_hash, SHA_DIGEST_LENGTH);
        close(fd);

        return bcmp(hash, piece_hash, SHA_DIGEST_LENGTH);
    }
}

static int
test_piece(struct torrent *tp, uint32_t pos, uint32_t piece, int *ok)
{
    int err;
    uint8_t hash[SHA_DIGEST_LENGTH];
    struct bt_stream *bts;
    if ((err = bts_open(&bts, &tp->meta, fd_cb_rd, tp)) != 0)
        return err;
    if ((err = bts_sha(bts, pos * tp->meta.piece_length,
             torrent_piece_size(tp, piece), hash)) != 0)
        return err;;
    bts_close(bts);
    *ok = test_hash(tp, hash, piece) == 0;
    return 0;
}

static void
cm_td_alloc(struct cm_op *op)
{
    struct torrent *tp = op->tp;
    struct content *cm = tp->cm;
    uint32_t pos = op->u.alloc.pos;
    struct bt_stream *bts;
    int err;

    assert(!has_bit(cm->pos_field, pos));

    if ((err = bts_open(&bts, &tp->meta, fd_cb_wr, tp)) != 0)
        goto out;

    off_t len = torrent_piece_size(tp, pos);
    off_t off = tp->meta.piece_length * pos;
    while (len > 0) {
        size_t wlen = min(ZEROBUFLEN, len);
        if ((err = bts_put(bts, off, m_zerobuf, wlen)) != 0) {
            bts_close(bts);
            goto out;
        }
        len -= wlen;
        off += wlen;
    }
    err = bts_close(bts);
out:
    if (err != 0)
        op->error = 1;
}

static int
test_torrent(struct torrent *tp, volatile sig_atomic_t *cancel)
{
    int err;
    FILE *fp;
    uint8_t (*hashes)[SHA_DIGEST_LENGTH];
    uint8_t hash[SHA_DIGEST_LENGTH];

    if ((err = vfopen(&fp, "r", "torrents/%s/torrent", tp->relpath)) != 0)
        return err;

    hashes = btpd_malloc(tp->meta.npieces * SHA_DIGEST_LENGTH);
    fseek(fp, tp->meta.pieces_off, SEEK_SET);
    fread(hashes, SHA_DIGEST_LENGTH, tp->meta.npieces, fp);
    fclose(fp);

    tp->meta.piece_hash = hashes;

    struct content *cm = tp->cm;
    for (uint32_t piece = 0; piece < tp->meta.npieces; piece++) {
        if (!has_bit(cm->pos_field, piece))
            continue;
        err = bts_sha(cm->rds, piece * tp->meta.piece_length,
            torrent_piece_size(tp, piece), hash);
        if (err != 0)
            break;
        if (test_hash(tp, hash, piece) == 0)
            set_bit(tp->cm->piece_field, piece);
        if (*cancel) {
            err = EINTR;
            break;
        }
    }

    tp->meta.piece_hash = NULL;
    free(hashes);
    return err;
}

struct rstat {
    time_t mtime;
    off_t size;
};

int
stat_and_adjust(struct torrent *tp, struct rstat ret[])
{
    char path[PATH_MAX];
    struct stat sb;
    for (int i = 0; i < tp->meta.nfiles; i++) {
        snprintf(path, PATH_MAX, "torrents/%s/content/%s", tp->relpath,
            tp->meta.files[i].path);
again:
        if (stat(path, &sb) == -1) {
            if (errno == ENOENT) {
                ret[i].mtime = -1;
                ret[i].size = -1;
            } else
                return errno;
        } else {
            ret[i].mtime = sb.st_mtime;
            ret[i].size = sb.st_size;
        }
        if (ret[i].size > tp->meta.files[i].length) {
            if (truncate(path, tp->meta.files[i].length) != 0)
                return errno;
            goto again;
        }
    }
    return 0;
}

static int
load_resume(struct torrent *tp, struct rstat sbs[])
{
    int err, ver;
    FILE *fp;
    size_t pfsiz = ceil(tp->meta.npieces / 8.0);
    size_t bfsiz = tp->meta.npieces * tp->cm->bppbf;

    if ((err = vfopen(&fp, "r" , "torrents/%s/resume", tp->relpath)) != 0)
        return err;

    if (fscanf(fp, "%d\n", &ver) != 1)
        goto invalid;
    if (ver != 1)
        goto invalid;
    for (int i = 0; i < tp->meta.nfiles; i++) {
        long long size;
        time_t time;
        if (fscanf(fp, "%qd %ld\n", &size, &time) != 2)
            goto invalid;
        if (sbs[i].size != size || sbs[i].mtime != time)
            err = EINVAL;
    }
    if (fread(tp->cm->piece_field, 1, pfsiz, fp) != pfsiz)
        goto invalid;
    if (fread(tp->cm->block_field, 1, bfsiz, fp) != bfsiz)
        goto invalid;
    fclose(fp);
    return err;
invalid:
    fclose(fp);
    bzero(tp->cm->piece_field, pfsiz);
    bzero(tp->cm->block_field, bfsiz);
    return EINVAL;
}

static int
save_resume(struct torrent *tp, struct rstat sbs[])
{
    int err;
    FILE *fp;
    if ((err = vfopen(&fp, "wb", "torrents/%s/resume", tp->relpath)) != 0)
        return err;
    fprintf(fp, "%d\n", 1);
    for (int i = 0; i < tp->meta.nfiles; i++)
        fprintf(fp, "%qd %ld\n", (long long)sbs[i].size, sbs[i].mtime);
    fwrite(tp->cm->piece_field, 1, ceil(tp->meta.npieces / 8.0), fp);
    fwrite(tp->cm->block_field, 1, tp->meta.npieces * tp->cm->bppbf, fp);
    if (fclose(fp) != 0)
        err = errno;
    return err;
}

static void
cm_td_save(struct cm_op *op)
{
    struct torrent *tp = op->tp;
    struct rstat sbs[tp->meta.nfiles];
    if (stat_and_adjust(tp, sbs) == 0)
        save_resume(tp, sbs);
}

static void
cm_td_start(struct cm_op *op)
{
    int err, resume_clean = 0, tested_torrent = 0;
    struct rstat sbs[op->tp->meta.nfiles];
    struct torrent *tp = op->tp;
    struct content *cm = tp->cm;

    if ((err = stat_and_adjust(op->tp, sbs)) != 0)
        goto out;

    resume_clean = load_resume(tp, sbs) == 0;
    if (!resume_clean) {
        memset(cm->pos_field, 0xff, ceil(tp->meta.npieces / 8.0));
        off_t off = 0;
        for (int i = 0; i < tp->meta.nfiles; i++) {
            if (sbs[i].size == -1 || sbs[i].size == 0) {
                uint32_t start = off / tp->meta.piece_length;
                uint32_t end = (off + tp->meta.files[i].length - 1) /
                    tp->meta.piece_length;
                while (start <= end) {
                    clear_bit(cm->pos_field, start);
                    clear_bit(cm->piece_field, start);
                    bzero(cm->block_field + start * cm->bppbf, cm->bppbf);
                    start++;
                }
            } else if (sbs[i].size < tp->meta.files[i].length) {
                uint32_t start = (off + sbs[i].size) /
                    tp->meta.piece_length;
                uint32_t end = (off + tp->meta.files[i].length - 1) /
                    tp->meta.piece_length;
                while (start <= end) {
                    clear_bit(cm->pos_field, start);
                    start++;
                }
            }
            off += tp->meta.files[i].length;
        }
        if (op->u.start.cancel)
            goto out;
        if ((err = test_torrent(tp, &op->u.start.cancel)) != 0)
            goto out;
        tested_torrent = 1;
    }

    bzero(cm->pos_field, ceil(tp->meta.npieces / 8.0));
    for (uint32_t piece = 0; piece < tp->meta.npieces; piece++) {
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
            resume_clean = 0;
            int ok = 0;
            if (!tested_torrent) {
                if (((err = test_piece(tp, piece, piece, &ok)) != 0
                        || op->u.start.cancel))
                    goto out;
            }
            if (ok) {
                set_bit(cm->pos_field, piece);
                set_bit(cm->piece_field, piece);
            } else
                bzero(bf, cm->bppbf);
        } else if (nblocks_got > 0)
            set_bit(cm->pos_field, piece);
    }

    if (!resume_clean)
        save_resume(tp, sbs);

out:
    if (!op->u.start.cancel && err != 0)
        op->error = 1;
}

static void
cm_td_test(struct cm_op *op)
{
    if (test_piece(op->tp, op->u.test.pos, op->u.test.piece,
            &op->u.test.ok) != 0)
        op->error = 1;
}

static void
cm_td_write(struct cm_op *op)
{
    int err;
    struct cm_write_data *d, *next;
    off_t base = op->tp->meta.piece_length * op->u.write.pos;
    struct bt_stream *bts;
    if ((err = bts_open(&bts, &op->tp->meta, fd_cb_wr, op->tp)) != 0)
        goto out;
    BTPDQ_FOREACH(d, &op->u.write.q, entry)
        if ((err = bts_put(bts, base + d->begin, d->buf, d->len)) != 0) {
            bts_close(bts);
            goto out;
        }
    err = bts_close(bts);
out:
    BTPDQ_FOREACH_MUTABLE(d, &op->u.write.q, entry, next)
        free(d);
    if (err)
        op->error = 1;
}

static void
cm_td(void *arg)
{
    struct cm_comm *comm = arg;
    struct cm_op *op;
    for (;;) {
        pthread_mutex_lock(&comm->lock);
        while (BTPDQ_EMPTY(&comm->q))
            pthread_cond_wait(&comm->cond, &comm->lock);

        op = BTPDQ_FIRST(&comm->q);
        BTPDQ_REMOVE(&comm->q, op, td_entry);
        op->received = 1;
        pthread_mutex_unlock(&comm->lock);

        switch (op->type) {
        case CM_ALLOC:
            cm_td_alloc(op);
            break;
        case CM_SAVE:
            cm_td_save(op);
            break;
        case CM_START:
            cm_td_start(op);
            break;
        case CM_TEST:
            cm_td_test(op);
            break;
        case CM_WRITE:
            cm_td_write(op);
            break;
        default:
            abort();
        }
        td_post_begin();
        td_post(cm_td_cb, op);
        td_post_end();
    }
}

void
cm_init(void)
{
    pthread_t td;
    BTPDQ_INIT(&m_long_comm.q);
    pthread_mutex_init(&m_long_comm.lock, NULL);
    pthread_cond_init(&m_long_comm.cond, NULL);
    pthread_create(&td, NULL, (void *(*)(void *))cm_td, &m_long_comm);
    BTPDQ_INIT(&m_short_comm.q);
    pthread_mutex_init(&m_short_comm.lock, NULL);
    pthread_cond_init(&m_short_comm.cond, NULL);
    pthread_create(&td, NULL, (void *(*)(void *))cm_td, &m_short_comm);
}
