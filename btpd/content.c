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

struct rstat {
    time_t mtime;
    off_t size;
};

struct content {
    enum { CM_INACTIVE, CM_STARTING, CM_ACTIVE } state;

    uint32_t npieces_got;

    off_t ncontent_bytes;

    size_t bppbf; // bytes per piece block field

    uint8_t *piece_field;
    uint8_t *block_field;
    uint8_t *pos_field;

    struct bt_stream *rds;
    struct bt_stream *wrs;

    struct event save_timer;
};

#define ZEROBUFLEN (1 << 14)

static const uint8_t m_zerobuf[ZEROBUFLEN];

int stat_and_adjust(struct torrent *tp, struct rstat ret[]);
static int save_resume(struct torrent *tp, struct rstat sbs[]);

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

struct pct_data {
    off_t off, remain;
    struct torrent *tp;
    SHA_CTX sha;
    BTPDQ_ENTRY(pct_data) entry;
    uint32_t piece;
    void (*cb)(struct torrent *, uint32_t, int);
};

BTPDQ_HEAD(pct_tq, pct_data);

static struct pct_tq m_pctq = BTPDQ_HEAD_INITIALIZER(m_pctq);
static void cm_write_done(struct torrent *tp);

struct start_test_data {
    struct torrent *tp;
    struct rstat *rstat;
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
    int fd;
    int err;

    err = vopen(&fd, O_RDONLY, "torrents/%s/torrent", tp->relpath);
    if (err != 0)
        btpd_err("failed to open 'torrents/%s/torrent' (%s).\n",
            tp->relpath, strerror(err));

    lseek(fd, tp->pieces_off + piece * SHA_DIGEST_LENGTH, SEEK_SET);
    read(fd, piece_hash, SHA_DIGEST_LENGTH);
    close(fd);

    return bcmp(hash, piece_hash, SHA_DIGEST_LENGTH);
}

void
pct_create(struct torrent *tp, uint32_t piece,
    void (*cb)(struct torrent *, uint32_t, int))
{
    struct pct_data *p = btpd_calloc(1, sizeof(*p));
    p->piece = piece;
    p->tp = tp;
    p->off = piece * tp->piece_length;
    p->remain = torrent_piece_size(tp, piece);
    SHA1_Init(&p->sha);
    p->cb = cb;
    BTPDQ_INSERT_TAIL(&m_pctq, p, entry);
    btpd_ev_add(&m_workev, (& (struct timeval) { 0, 0 }));
}

void
pct_kill(struct pct_data *p)
{
    BTPDQ_REMOVE(&m_pctq, p, entry);
    free(p);
}

void
pct_run(struct pct_data *p)
{
    char buf[READBUFLEN];
    size_t unit = (10 << 14);

    while (p->remain > 0 && unit > 0) {
        size_t wantread = min(p->remain, sizeof(buf));
        if (wantread > unit)
            wantread = unit;
        if ((errno = bts_get(p->tp->cm->rds, p->off, buf, wantread)) != 0)
            btpd_err("IO error on '%s' (%s).\n", bts_filename(p->tp->cm->rds),
                strerror(errno));
        p->remain -= wantread;
        unit -= wantread;
        p->off += wantread;
        SHA1_Update(&p->sha, buf, wantread);
    }
    if (p->remain == 0) {
        uint8_t hash[SHA_DIGEST_LENGTH];
        SHA1_Final(hash, &p->sha);
        p->cb(p->tp, p->piece, test_hash(p->tp, hash, p->piece) == 0);
        pct_kill(p);
    }
}

void
pct_cb(struct torrent *tp, uint32_t piece, int ok)
{
    struct content *cm = tp->cm;
    if (ok) {
        assert(cm->npieces_got < tp->npieces);
        cm->npieces_got++;
        set_bit(cm->piece_field, piece);
        if (net_active(tp))
            dl_on_ok_piece(tp->net, piece);
        if (cm_full(tp))
            cm_write_done(tp);
    } else {
        cm->ncontent_bytes -= torrent_piece_size(tp, piece);
        bzero(cm->block_field + piece * cm->bppbf, cm->bppbf);
        if (net_active(tp))
            dl_on_bad_piece(tp->net, piece);
    }
}

void
work_stop(struct torrent *tp)
{
    struct content *cm = tp->cm;
    struct pct_data *pct, *next;
    if (cm->state == CM_STARTING) {
        struct start_test_data *std;
        BTPDQ_FOREACH(std, &m_startq, entry)
            if (std->tp == tp) {
                BTPDQ_REMOVE(&m_startq, std, entry);
                free(std->rstat);
                free(std);
                break;
            }
    }
    BTPDQ_FOREACH_MUTABLE(pct, &m_pctq, entry, next)
        if (pct->tp == tp)
            pct_kill(pct);
}

static int test_hash(struct torrent *tp, uint8_t *hash, uint32_t piece);

void
worker_cb(int fd, short type, void *arg)
{
    struct pct_data *p = BTPDQ_FIRST(&m_pctq);
    if (p == NULL)
        return;
    pct_run(p);
    if (!BTPDQ_EMPTY(&m_pctq))
        event_add(&m_workev, (& (struct timeval) { 0, 0 }));
}

void
cm_kill(struct torrent *tp)
{
    struct content *cm = tp->cm;
    bts_close(cm->rds);
    free(cm->piece_field);
    free(cm->block_field);
    free(cm->pos_field);
    free(cm);
    tp->cm = NULL;
}

void
cm_save(struct torrent *tp)
{
    struct rstat sbs[tp->nfiles];
    if (stat_and_adjust(tp, sbs) == 0)
        save_resume(tp, sbs);
}

static void
cm_write_done(struct torrent *tp)
{
    struct content *cm = tp->cm;

    if ((errno = bts_close(cm->wrs)) != 0)
        btpd_err("error closing write stream for '%s' (%s).\n",
            torrent_name(tp), strerror(errno));
    cm->wrs = NULL;
    btpd_ev_del(&cm->save_timer);
    cm_save(tp);
}

void
cm_stop(struct torrent *tp)
{
    struct content *cm = tp->cm;

    if (cm->state == CM_ACTIVE && !cm_full(tp))
        cm_write_done(tp);

    work_stop(tp);

    cm->state = CM_INACTIVE;
}

int
cm_active(struct torrent *tp)
{
    struct content *cm = tp->cm;
    return cm->state != CM_INACTIVE;
}

int
cm_started(struct torrent *tp)
{
    struct content *cm = tp->cm;
    return cm->state == CM_ACTIVE;
}

#define SAVE_INTERVAL (& (struct timeval) { 15, 0 })

static void
save_timer_cb(int fd, short type, void *arg)
{
    struct torrent *tp = arg;
    btpd_ev_add(&tp->cm->save_timer, SAVE_INTERVAL);
    cm_save(tp);
}

void
cm_create(struct torrent *tp, const char *mi)
{
    size_t pfield_size = ceil(tp->npieces / 8.0);
    struct content *cm = btpd_calloc(1, sizeof(*cm));
    cm->bppbf = ceil((double)tp->piece_length / (1 << 17));
    cm->piece_field = btpd_calloc(pfield_size, 1);
    cm->pos_field = btpd_calloc(pfield_size, 1);
    cm->block_field = btpd_calloc(tp->npieces * cm->bppbf, 1);

    evtimer_set(&cm->save_timer, save_timer_cb, tp);

    tp->cm = cm;
}

int
cm_get_bytes(struct torrent *tp, uint32_t piece, uint32_t begin, size_t len,
    uint8_t **buf)
{
    *buf = btpd_malloc(len);
    int err =
        bts_get(tp->cm->rds, piece * tp->piece_length + begin, *buf, len);
    if (err != 0)
        btpd_err("IO error on '%s' (%s).\n", bts_filename(tp->cm->rds),
            strerror(err));
    return 0;
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
    pct_create(tp, piece, pct_cb);
}

int
cm_put_bytes(struct torrent *tp, uint32_t piece, uint32_t begin,
    const uint8_t *buf, size_t len)
{
    int err;
    struct content *cm = tp->cm;

    if (!has_bit(cm->pos_field, piece)) {
        unsigned npieces = ceil((double)cm_alloc_size / tp->piece_length);
        uint32_t start = piece - piece % npieces;
        uint32_t end = min(start + npieces, tp->npieces);

        while (start < end) {
            if (!has_bit(cm->pos_field, start)) {
                off_t len = torrent_piece_size(tp, start);
                off_t off = tp->piece_length * start;
                while (len > 0) {
                    size_t wlen = min(ZEROBUFLEN, len);
                    if ((err = bts_put(cm->wrs, off, m_zerobuf, wlen)) != 0)
                        btpd_err("IO error on '%s' (%s).\n",
                            bts_filename(cm->wrs), strerror(errno));

                    len -= wlen;
                    off += wlen;
                }
                set_bit(cm->pos_field, start);
            }
            start++;
        }
    }
    err = bts_put(cm->wrs, piece * tp->piece_length + begin, buf, len);
    if (err != 0)
        btpd_err("IO error on '%s' (%s)\n", bts_filename(cm->wrs),
            strerror(err));

    cm->ncontent_bytes += len;
    uint8_t *bf = cm->block_field + piece * cm->bppbf;
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
stat_and_adjust(struct torrent *tp, struct rstat ret[])
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
                if (errno != 0 || close(fd) != 0)
                    return errno;
                goto again;
            } else
                return errno;
        } else if (sb.st_size > tp->files[i].length) {
            if (truncate(path, tp->files[i].length) != 0)
                return errno;
            goto again;
        } else {
            ret[i].mtime = sb.st_mtime;
            ret[i].size = sb.st_size;
        }
    }
    return 0;
}

static int
load_resume(struct torrent *tp, struct rstat sbs[])
{
    int err, ver;
    FILE *fp;
    size_t pfsiz = ceil(tp->npieces / 8.0);
    size_t bfsiz = tp->npieces * tp->cm->bppbf;

    if ((err = vfopen(&fp, "r" , "torrents/%s/resume", tp->relpath)) != 0)
        return err;

    if (fscanf(fp, "%d\n", &ver) != 1)
        goto invalid;
    if (ver != 1)
        goto invalid;
    for (int i = 0; i < tp->nfiles; i++) {
        quad_t size;
        long time;
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
    for (int i = 0; i < tp->nfiles; i++)
        fprintf(fp, "%lld %ld\n", (long long)sbs[i].size, (long)sbs[i].mtime);
    fwrite(tp->cm->piece_field, 1, ceil(tp->npieces / 8.0), fp);
    fwrite(tp->cm->block_field, 1, tp->npieces * tp->cm->bppbf, fp);
    if (fclose(fp) != 0)
        err = errno;
    return err;
}

void start_test_cb(struct torrent *tp, uint32_t piece, int ok);

void
start_test_end(struct torrent *tp, int unclean)
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
    if (!cm_full(tp)) {
        int err;
        if ((err = bts_open(&cm->wrs, tp->nfiles, tp->files,
                 fd_cb_wr, tp)) != 0)
            btpd_err("failed to open write stream for '%s' (%s).\n",
                torrent_name(tp), strerror(err));
        btpd_ev_add(&cm->save_timer, SAVE_INTERVAL);
    }
    if (unclean) {
        struct start_test_data *std = BTPDQ_FIRST(&m_startq);

        assert(std->tp == tp);
        BTPDQ_REMOVE(&m_startq, std, entry);
        save_resume(tp, std->rstat);
        free(std->rstat);
        free(std);

        if ((std = BTPDQ_FIRST(&m_startq)) != NULL)
            pct_create(std->tp, std->start, start_test_cb);
    }
    cm->state = CM_ACTIVE;
}

void
start_test_cb(struct torrent *tp, uint32_t piece, int ok)
{
    struct content *cm = tp->cm;
    if (ok)
        set_bit(cm->piece_field, piece);
    else
        clear_bit(cm->piece_field, piece);
    piece++;
    while (piece < tp->npieces && !has_bit(cm->pos_field, piece))
        piece++;
    if (piece < tp->npieces)
        pct_create(tp, piece, start_test_cb);
    else
        start_test_end(tp, 1);
}

void
start_test(struct torrent *tp, struct rstat *sbs)
{
    uint32_t piece = 0;
    struct content *cm = tp->cm;
    while (piece < tp->npieces && !has_bit(cm->pos_field, piece))
        piece++;
    if (piece < tp->npieces) {
        struct start_test_data *std = btpd_calloc(1, sizeof(*std));
        std->tp = tp;
        std->start = piece;
        std->rstat = sbs;
        BTPDQ_INSERT_TAIL(&m_startq, std, entry);
        if (std == BTPDQ_FIRST(&m_startq))
            pct_create(tp, piece, start_test_cb);
    } else {
        free(sbs);
        start_test_end(tp, 0);
    }
}

void
cm_start(struct torrent *tp)
{
    int err, resume_clean = 0;
    struct rstat *sbs;
    struct content *cm = tp->cm;

    if ((errno = bts_open(&cm->rds, tp->nfiles, tp->files, fd_cb_rd, tp)) != 0)
        btpd_err("failed to open stream for '%s' (%s).\n",
            torrent_name(tp), strerror(errno));

    cm->state = CM_STARTING;

    sbs = btpd_calloc(tp->nfiles, sizeof(*sbs));

    if ((err = stat_and_adjust(tp, sbs)) != 0)
        btpd_err("failed stat_and_adjust for '%s' (%s).\n",
            torrent_name(tp), strerror(err));

    resume_clean = load_resume(tp, sbs) == 0;
    if (!resume_clean) {
        memset(cm->pos_field, 0xff, ceil(tp->npieces / 8.0));
        off_t off = 0;
        for (int i = 0; i < tp->nfiles; i++) {
            if (sbs[i].size != tp->files[i].length) {
                uint32_t start, end;
                end = (off + tp->files[i].length - 1)
                    / tp->piece_length;
                start = (off + sbs[i].size) / tp->piece_length;
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
    for (uint32_t piece = 0; piece < tp->npieces; piece++) {
        if (has_bit(cm->piece_field, piece))
            continue;
        uint8_t *bf = cm->block_field + cm->bppbf * piece;
        uint32_t nblocks = torrent_piece_blocks(tp, piece);
        uint32_t block = 0;
        while (block < nblocks && has_bit(bf, block))
            block++;
        if (block == nblocks)
            set_bit(cm->pos_field, piece);
    }

    start_test(tp, sbs);
}

void
cm_init(void)
{
    evtimer_set(&m_workev, worker_cb, NULL);
}
