#include <math.h>

#include "btpd.h"

struct content {
    uint32_t npieces;
    uint8_t *piece_field;

    uint64_t nblocks;
    uint8_t *block_field;

    uint32_t *piece_map;

    struct event done;
};

void
done_cb(int fd, short type, void *arg)
{
    struct torrent *tp = arg;
    if (tp->state == T_STARTING)
        torrent_cm_cb(tp, CM_STARTED);
    else
        torrent_cm_cb(tp, CM_STOPPED);
}

int
cm_start(struct torrent *tp)
{
    size_t mem =
        sizeof(struct content)
        + sizeof(uint32_t) * tp->meta.npieces
        + ceil(tp->meta.npieces / 8.0)
        + tp->meta.npieces * ceil(tp->meta.piece_length / (double)(1 << 17));

    tp->cm = btpd_calloc(mem, 1);
    tp->cm->piece_map = (uint32_t *)(tp->cm + 1);
    tp->cm->piece_field = (uint8_t *)(tp->cm->piece_map + tp->meta.npieces);
    tp->cm->block_field = tp->cm->piece_field
        + (size_t)ceil(tp->meta.npieces / 8.0);

    evtimer_set(&tp->cm->done, done_cb, tp);
    evtimer_add(&tp->cm->done, (& (struct timeval) { 0, 0 }));

    return 0;
}

void
cm_stop(struct torrent *tp)
{
    evtimer_add(&tp->cm->done, (& (struct timeval) { 0, 0 }));
}

int
cm_full(struct torrent *tp)
{
    return tp->cm->npieces == tp->meta.npieces;
}

uint8_t *
cm_get_piece_field(struct torrent *tp)
{
    return tp->cm->piece_field;
}

uint8_t *
cm_get_block_field(struct torrent *tp, uint32_t piece)
{
    return tp->cm->block_field +
        piece * (size_t)ceil(tp->meta.piece_length / (double)(1 << 17));
}

int
cm_has_piece(struct torrent *tp, uint32_t piece)
{
    return has_bit(tp->cm->piece_field, piece);
}

int
cm_put_block(struct torrent *tp, uint32_t piece, uint32_t block,
    const char *data)
{
    set_bit(tp->cm->block_field +
        piece * (size_t)ceil(tp->meta.piece_length / (double)(1 << 17)),
        block);
    tp->cm->nblocks++;
    return 0;
}

int
cm_get_bytes(struct torrent *tp, uint32_t piece, uint32_t begin, size_t len,
    char **buf)
{
    *buf = btpd_malloc(len);
    return 0;
}

struct test {
    struct piece *pc;
    struct event test;
};

static
void test_cb(int fd, short type, void *arg)
{
    struct test *t = arg;
    set_bit(t->pc->n->tp->cm->piece_field, t->pc->index);
    t->pc->n->tp->cm->npieces++;
    dl_on_ok_piece(t->pc);
    free(t);
}

void
cm_test_piece(struct piece *pc)
{
    struct test *t = btpd_calloc(1, sizeof(*t));
    t->pc = pc;
    evtimer_set(&t->test, test_cb, t);
    evtimer_add(&t->test, (& (struct timeval) { 0 , 0 }));
}

uint32_t
cm_get_npieces(struct torrent *tp)
{
    return tp->cm->npieces;
}

off_t
cm_bytes_left(struct torrent *tp)
{
    return cm_full(tp) ? 0 : tp->meta.total_length;
}
