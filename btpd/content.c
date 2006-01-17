#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "btpd.h"
#include "stream.h"

struct data {
    uint32_t begin;
    uint8_t *buf;
    size_t len;
    BTPDQ_ENTRY(data) entry;
};

BTPDQ_HEAD(data_tq, data);

enum cm_op_type {
    CM_ALLOC,
    CM_TEST,
    CM_WRITE
};

struct cm_op {
    struct torrent *tp;

    enum cm_op_type type;
    union {
        struct {
            uint32_t piece;
            uint32_t pos;
        } alloc;
        struct {
            uint32_t piece;
            uint32_t pos;
            int ok;
        } test;
        struct {
            uint32_t piece;
            uint32_t pos;
            struct data_tq q;
        } write;
    } u;

    int error;
    char *errmsg;

    BTPDQ_ENTRY(cm_op) cm_entry;
    BTPDQ_ENTRY(cm_op) td_entry;
};

BTPDQ_HEAD(cm_op_tq, cm_op);

struct content {
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
};

#define ZEROBUFLEN (1 << 14)

static const uint8_t m_zerobuf[ZEROBUFLEN];
static struct cm_op_tq m_tdq = BTPDQ_HEAD_INITIALIZER(m_tdq);
static pthread_mutex_t m_tdq_lock;
static pthread_cond_t m_tdq_cond;

static int
fd_cb_rd(const char *path, int *fd, void *arg)
{
    struct torrent *tp = arg;
    return vopen(fd, O_RDONLY, "library/%s/content/%s", tp->relpath, path);
}

static int
fd_cb_wr(const char *path, int *fd, void *arg)
{
    struct torrent *tp = arg;
    return vopen(fd, O_RDWR|O_CREAT, "library/%s/content/%s", tp->relpath,
        path);
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

    pthread_mutex_lock(&m_tdq_lock);
    BTPDQ_INSERT_TAIL(&m_tdq, op, td_entry);
    pthread_mutex_unlock(&m_tdq_lock);
    pthread_cond_signal(&m_tdq_cond);
}

static void
cm_td_cb(void *arg)
{
    struct cm_op *op = arg;
    struct torrent *tp = op->tp;
    struct content *cm = tp->cm;

    if (op->error)
        btpd_err("%s", op->errmsg);

    switch (op->type) {
    case CM_ALLOC:
        set_bit(cm->pos_field, op->u.alloc.pos);
        clear_bit(cm->hold_field, op->u.alloc.piece);
        break;
    case CM_TEST:
        if (op->u.test.ok) {
            cm->npieces_got++;
            set_bit(cm->piece_field, op->u.test.piece);
            if (tp->net != NULL)
                dl_on_ok_piece(op->tp->net, op->u.test.piece);
        } else {
            cm->ncontent_bytes -= torrent_piece_size(tp, op->u.test.piece);
            bzero(cm->block_field + op->u.test.piece * cm->bppbf, cm->bppbf);
            if (tp->net != NULL)
                dl_on_bad_piece(tp->net, op->u.test.piece);
        }
        break;
    default:
        break;
    }
    BTPDQ_REMOVE(&cm->todoq, op, cm_entry);
    free(op);
    if (!BTPDQ_EMPTY(&cm->todoq))
        run_todo(cm);
}

static int
test_hash(struct torrent *tp, uint8_t *hash, uint32_t index)
{
    if (tp->meta.piece_hash != NULL)
        return bcmp(hash, tp->meta.piece_hash[index], SHA_DIGEST_LENGTH);
    else {
        char piece_hash[SHA_DIGEST_LENGTH];
        int fd;
        int err;

        err = vopen(&fd, O_RDONLY, "library/%s/torrent", tp->relpath);
        if (err != 0)
            btpd_err("test_hash: %s\n", strerror(err));

        lseek(fd, tp->meta.pieces_off + index * SHA_DIGEST_LENGTH, SEEK_SET);
        read(fd, piece_hash, SHA_DIGEST_LENGTH);
        close(fd);

        return bcmp(hash, piece_hash, SHA_DIGEST_LENGTH);
    }
}

static void
cm_td_alloc(struct cm_op *op)
{
    struct bt_stream *bts;
    off_t len = torrent_piece_size(op->tp, op->u.alloc.pos);
    off_t off = op->tp->meta.piece_length * op->u.alloc.pos;
    bts_open(&bts, &op->tp->meta, fd_cb_wr, op->tp);
    while (len > 0) {
        size_t wlen = min(ZEROBUFLEN, len);
        bts_put(bts, off, m_zerobuf, wlen);
        len -= wlen;
        off += wlen;
    }
    bts_close(bts);
}

static void
cm_td_test(struct cm_op *op)
{
    uint8_t hash[SHA_DIGEST_LENGTH];
    struct bt_stream *bts;
    bts_open(&bts, &op->tp->meta, fd_cb_rd, op->tp);
    bts_sha(bts, op->u.test.pos * op->tp->meta.piece_length,
        torrent_piece_size(op->tp, op->u.test.piece), hash);
    bts_close(bts);
    op->u.test.ok = test_hash(op->tp, hash, op->u.test.piece) == 0;
}

static void
cm_td_write(struct cm_op *op)
{
    struct data *d, *next;
    off_t base = op->tp->meta.piece_length * op->u.write.pos;
    struct bt_stream *bts;
    bts_open(&bts, &op->tp->meta, fd_cb_wr, op->tp);
    BTPDQ_FOREACH(d, &op->u.write.q, entry)
        bts_put(bts, base + d->begin, d->buf, d->len);
    bts_close(bts);
    BTPDQ_FOREACH_MUTABLE(d, &op->u.write.q, entry, next)
        free(d);
}

static void
cm_td(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&m_tdq_lock);
        while (BTPDQ_EMPTY(&m_tdq))
            pthread_cond_wait(&m_tdq_cond, &m_tdq_lock);
        struct cm_op *op = BTPDQ_FIRST(&m_tdq);
        BTPDQ_REMOVE(&m_tdq, op, td_entry);
        pthread_mutex_unlock(&m_tdq_lock);

        switch (op->type) {
        case CM_ALLOC:
            cm_td_alloc(op);
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
    pthread_mutex_init(&m_tdq_lock, NULL);
    pthread_cond_init(&m_tdq_cond, NULL);
    pthread_create(&td, NULL, (void *(*)(void *))cm_td, NULL);
}

int
cm_start(struct torrent *tp)
{
    int err;
    struct content *cm = btpd_calloc(1, sizeof(*cm));
    size_t pfield_size = ceil(tp->meta.npieces / 8.0);
    cm->bppbf = ceil((double)tp->meta.piece_length / (1 << 17));
    cm->piece_field = btpd_calloc(pfield_size, 1);
    cm->hold_field = btpd_calloc(pfield_size, 1);
    cm->pos_field = btpd_calloc(pfield_size, 1);
    cm->block_field = btpd_calloc(tp->meta.npieces * cm->bppbf, 1);

    BTPDQ_INIT(&cm->todoq);

    if ((err = bts_open(&cm->rds, &tp->meta, fd_cb_rd, tp)) != 0)
        btpd_err("Error opening stream (%s).\n", strerror(err));
    if ((err = bts_open(&cm->wrs, &tp->meta, fd_cb_wr, tp)) != 0)
        btpd_err("Error opening stream (%s).\n", strerror(err));

    tp->cm = cm;
    torrent_cm_cb(tp, CM_STARTED);
    return 0;
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

void
cm_prealloc(struct torrent *tp, uint32_t piece)
{
    struct content *cm = tp->cm;
    if (has_bit(cm->pos_field, piece))
        return;

    set_bit(cm->hold_field, piece);

    int was_empty = BTPDQ_EMPTY(&cm->todoq);
    struct cm_op *op = btpd_calloc(1, sizeof(*op));
    op->tp = tp;
    op->type = CM_ALLOC;
    op->u.alloc.piece = piece;
    op->u.alloc.pos = piece;
    BTPDQ_INSERT_TAIL(&cm->todoq, op, cm_entry);

    op = btpd_calloc(1, sizeof(*op));
    op->tp = tp;
    op->type = CM_WRITE;
    op->u.write.piece = piece;
    op->u.write.pos = piece;
    BTPDQ_INIT(&op->u.write.q);
    BTPDQ_INSERT_TAIL(&cm->todoq, op, cm_entry);

    if (was_empty)
        run_todo(cm);
}

void
cm_test_piece(struct torrent *tp, uint32_t piece)
{
    struct content *cm = tp->cm;
    int was_empty = BTPDQ_EMPTY(&cm->todoq);
    struct cm_op *op = btpd_calloc(1, sizeof(*op));
    op->tp = tp;
    op->type = CM_TEST;
    op->u.test.piece = piece;
    op->u.test.pos = piece;
    BTPDQ_INSERT_TAIL(&cm->todoq, op, cm_entry);
    if (was_empty)
        run_todo(cm);
}

int
cm_put_bytes(struct torrent *tp, uint32_t piece, uint32_t begin,
    const uint8_t *buf, size_t len)
{
    int err;
    struct content *cm = tp->cm;

    if (has_bit(cm->hold_field, piece)) {
        struct data *d = btpd_calloc(1, sizeof(*d) + len);
        d->begin = begin;
        d->len = len;
        d->buf = (uint8_t *)(d + 1);
        bcopy(buf, d->buf, len);
        struct cm_op *op;
        BTPDQ_FOREACH(op, &cm->todoq, cm_entry)
            if (op->type == CM_WRITE && op->u.write.piece == piece)
                break;
        struct data *it;
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
