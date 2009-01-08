#include "btpd.h"

#include <sys/mman.h>
#include <dirent.h>
#include <iobuf.h>

HTBL_TYPE(numtbl, tlib, unsigned, num, nchain);
HTBL_TYPE(hashtbl, tlib, uint8_t, hash, hchain);

static unsigned m_nextnum;
static unsigned m_ntlibs;
static struct numtbl *m_numtbl;
static struct hashtbl *m_hashtbl;

unsigned
tlib_count(void)
{
    return m_ntlibs;
}

struct tlib *
tlib_by_num(unsigned num)
{
    return numtbl_find(m_numtbl, &num);
}

struct tlib *
tlib_by_hash(const uint8_t *hash)
{
    return hashtbl_find(m_hashtbl, hash);
}

void
tlib_kill(struct tlib *tl)
{
    numtbl_remove(m_numtbl, &tl->num);
    hashtbl_remove(m_hashtbl, tl->hash);
    if (tl->name != NULL)
        free(tl->name);
    if (tl->dir != NULL)
        free(tl->dir);
    free(tl);
    m_ntlibs--;
}

struct tlib *
tlib_create(const uint8_t *hash)
{
    struct tlib *tl = btpd_calloc(1, sizeof(*tl));
    char hex[SHAHEXSIZE];
    bin2hex(hash, hex, 20);
    tl->num = m_nextnum;
    bcopy(hash, tl->hash, 20);
    m_nextnum++;
    m_ntlibs++;
    numtbl_insert(m_numtbl, tl);
    hashtbl_insert(m_hashtbl, tl);
    return tl;
}

int
tlib_del(struct tlib *tl)
{
    char relpath[RELPATH_SIZE];
    char path[PATH_MAX];
    DIR *dir;
    struct dirent *de;
    assert(tl->tp == NULL);
    snprintf(path, PATH_MAX, "torrents/%s", bin2hex(tl->hash, relpath, 20));
    if ((dir = opendir(path)) != NULL) {
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(".", de->d_name) == 0 || strcmp("..", de->d_name) == 0)
                continue;
            snprintf(path, PATH_MAX, "torrents/%s/%s", relpath, de->d_name);
            remove(path);
        }
        closedir(dir);
    }
    snprintf(path, PATH_MAX, "torrents/%s", relpath);
    remove(path);
    tlib_kill(tl);
    return 0;
}

static void
dct_subst_save(FILE *fp, const char *dct1, const char *dct2)
{
    fprintf(fp, "d");
    const char *k1 = benc_first(dct1), *k2 = benc_first(dct2);
    const char *val, *str, *rest;
    size_t len;

    while (k1 != NULL && k2 != NULL) {
        int test = benc_strcmp(k1, k2);
        if (test < 0) {
            str = benc_mem(k1, &len, &val);
            fprintf(fp, "%d:%.*s", (int)len, (int)len, str);
            fwrite(val, 1, benc_length(val), fp);
            k1 = benc_next(val);
        } else {
            str = benc_mem(k2, &len, &val);
            fprintf(fp, "%d:%.*s", (int)len, (int)len, str);
            fwrite(val, 1, benc_length(val), fp);
            k2 = benc_next(val);
            if (test == 0)
                k1 = benc_next(benc_next(k1));
        }
    }
    rest = k1 != NULL ? k1 : k2;
    while (rest != NULL) {
        str = benc_mem(rest, &len, &val);
        fprintf(fp, "%d:%.*s", (int)len, (int)len, str);
        fwrite(val, 1, benc_length(val), fp);
        rest = benc_next(val);
    }
    fprintf(fp, "e");
}

static int
valid_info(char *buf, size_t len)
{
    size_t slen;
    const char *info;
    if (benc_validate(buf, len) != 0)
        return 0;
    if ((info = benc_dget_dct(buf, "info")) == NULL)
        return 0;
    if (benc_dget_mem(info, "name", &slen) == NULL || slen == 0)
        return 0;
    if ((benc_dget_mem(info, "dir", &slen) == NULL ||
            (slen == 0 || slen >= PATH_MAX)))
        return 0;
    return 1;
}

static void
load_info(struct tlib *tl, const char *path)
{
    size_t size = 1 << 14;
    char buf[size];
    const char *info;

    if (read_file(path, buf, &size) == NULL) {
        btpd_log(BTPD_L_ERROR, "couldn't load '%s' (%s).\n", path,
            strerror(errno));
        return;
    }

    if (!valid_info(buf, size)) {
        btpd_log(BTPD_L_ERROR, "bad info file '%s'.\n", path);
        return;
    }

    info = benc_dget_dct(buf, "info");
    tl->name = benc_dget_str(info, "name", NULL);
    tl->dir = benc_dget_str(info, "dir", NULL);
    tl->tot_up = benc_dget_int(info, "total upload");
    tl->tot_down = benc_dget_int(info, "total download");
    tl->content_size = benc_dget_int(info, "content size");
    tl->content_have = benc_dget_int(info, "content have");
    if (tl->name == NULL || tl->dir == NULL)
        btpd_err("Out of memory.\n");
}

static void
save_info(struct tlib *tl)
{
    FILE *fp;
    char relpath[SHAHEXSIZE], path[PATH_MAX], wpath[PATH_MAX];
    struct iobuf iob = iobuf_init(1 << 10);

    iobuf_print(&iob,
        "d4:infod"
        "12:content havei%llde12:content sizei%llde"
        "3:dir%d:%s4:name%d:%s"
        "14:total downloadi%llde12:total uploadi%llde"
        "ee",
        (long long)tl->content_have, (long long)tl->content_size,
        (int)strlen(tl->dir), tl->dir, (int)strlen(tl->name), tl->name,
        tl->tot_down, tl->tot_up);
    if (iob.error)
        btpd_err("Out of memory.\n");

    bin2hex(tl->hash, relpath, 20);
    snprintf(path, PATH_MAX, "torrents/%s/info", relpath);
    snprintf(wpath, PATH_MAX, "%s.write", path);

    if ((fp = fopen(wpath, "w")) == NULL)
        btpd_err("failed to open '%s' (%s).\n", wpath, strerror(errno));
    dct_subst_save(fp, "de", iob.buf);
    iobuf_free(&iob);
    if ((fflush(fp) == EOF || fsync(fileno(fp)) != 0
            || ferror(fp) || fclose(fp) != 0))
        btpd_err("failed to write '%s'.\n", wpath);
    if (rename(wpath, path) != 0)
        btpd_err("failed to rename: '%s' -> '%s' (%s).\n", wpath, path,
            strerror(errno));
}

void
tlib_update_info(struct tlib *tl, int only_file)
{
    struct tlib tmp;
    assert(tl->tp != NULL);
    if (only_file) {
        tmp = *tl;
        tl = &tmp;
    }
    tl->tot_down += tl->tp->net->downloaded;
    tl->tot_up += tl->tp->net->uploaded;
    tl->content_have = cm_content(tl->tp);
    tl->content_size = tl->tp->total_length;
    save_info(tl);
}

static void
write_torrent(const char *mi, size_t mi_size, const char *path)
{
    FILE *fp;
    if ((fp = fopen(path, "w")) == NULL)
        goto err;
    if (fwrite(mi, mi_size, 1, fp) != 1) {
        errno = EIO;
        goto err;
    }
    if (fclose(fp) != 0)
        goto err;
    return;
err:
    btpd_err("failed to write metainfo '%s' (%s).\n", path, strerror(errno));
}

struct tlib *
tlib_add(const uint8_t *hash, const char *mi, size_t mi_size,
    const char *content, char *name)
{
    struct tlib *tl = tlib_create(hash);
    char relpath[RELPATH_SIZE], file[PATH_MAX];
    bin2hex(hash, relpath, 20);

    if (name == NULL)
        if ((name = mi_name(mi)) == NULL)
            btpd_err("out of memory.\n");

    tl->content_size = mi_total_length(mi);
    tl->name = name;
    tl->dir = strdup(content);
    if (tl->name == NULL || tl->dir == NULL)
        btpd_err("out of memory.\n");

    snprintf(file, PATH_MAX, "torrents/%s", relpath);
    if (mkdir(file, 0777) != 0)
        btpd_err("failed to create dir '%s' (%s).\n", file, strerror(errno));
    snprintf(file, PATH_MAX, "torrents/%s/torrent", relpath);
    write_torrent(mi, mi_size, file);
    save_info(tl);
    return tl;
}

static int
num_test(const void *k1, const void *k2)
{
    return *(const unsigned *)k1 == *(const unsigned *)k2;
}

static uint32_t
num_hash(const void *k)
{
    return *(const unsigned *)k;
}

static int
id_test(const void *k1, const void *k2)
{
    return bcmp(k1, k2, 20) == 0;
}

static uint32_t
id_hash(const void *k)
{
    return dec_be32(k + 16);
}

void
tlib_put_all(struct tlib **v)
{
    hashtbl_tov(m_hashtbl, v);
}

void
tlib_init(void)
{
    DIR *dirp;
    struct dirent *dp;
    uint8_t hash[20];
    char file[PATH_MAX];

    m_numtbl = numtbl_create(num_test, num_hash);
    m_hashtbl = hashtbl_create(id_test, id_hash);
    if (m_numtbl == NULL || m_hashtbl == NULL)
        btpd_err("Out of memory.\n");

    if ((dirp = opendir("torrents")) == NULL)
        btpd_err("couldn't open the torrents directory.\n");
    while ((dp = readdir(dirp)) != NULL) {
        if (strlen(dp->d_name) == 40 && ishex(dp->d_name)) {
            struct tlib * tl = tlib_create(hex2bin(dp->d_name, hash, 20));
            snprintf(file, PATH_MAX, "torrents/%s/info", dp->d_name);
            load_info(tl, file);
        }
    }
    closedir(dirp);
}

void
tlib_read_hash(struct tlib *tl, size_t off, uint32_t piece, uint8_t *hash)
{
    int fd;
    ssize_t nread;
    char relpath[RELPATH_SIZE];
    bin2hex(tl->hash, relpath, 20);

    if ((errno = vopen(&fd, O_RDONLY, "torrents/%s/torrent", relpath)) != 0)
        btpd_err("failed to open 'torrents/%s/torrent' (%s).\n",
            relpath, strerror(errno));
    lseek(fd, off + piece * 20, SEEK_SET);
    if ((nread = read(fd, hash, 20)) != 20) {
        if (nread == -1)
            btpd_err("failed to read 'torrents/%s/torrent' (%s).\n", relpath,
                strerror(errno));
        else
            btpd_err("corrupt file: 'torrents/%s/torrent'.\n", relpath);
    }

    close(fd);
}

int
tlib_load_mi(struct tlib *tl, char **res)
{
    char file[PATH_MAX];
    char relpath[RELPATH_SIZE];
    char *mi;
    bin2hex(tl->hash, relpath, 20);
    snprintf(file, sizeof(file), "torrents/%s/torrent", relpath);
    if ((mi = mi_load(file, NULL)) == NULL) {
        btpd_log(BTPD_L_ERROR,
            "torrent '%s': failed to load metainfo (%s).\n",
            tl->name, strerror(errno));
        return errno;
    }
    *res = mi;
    return 0;
}

struct resume_data {
    void *base;
    size_t size;
    uint8_t *pc_field;
    uint8_t *blk_field;
};

static void *
resume_file_size(struct resume_data *resd, int i)
{
    return resd->base + 8 + 16 * i;
}

static void *
resume_file_time(struct resume_data *resd, int i)
{
    return resd->base + 16 + 16 * i;
}

static void
init_resume(int fd, size_t size)
{
    char buf[1024];
    uint32_t ver;
    bzero(buf, sizeof(buf));
    enc_be32(&ver, 2);
    if (write(fd, "RESD", 4) == -1 || write(fd, &ver, 4) == -1)
        goto fatal;
    size -= 8;
    while (size > 0) {
        ssize_t nw = write(fd, buf, min(sizeof(buf), size));
        if (nw < 1)
            goto fatal;
        size -= nw;
    }
    return;
fatal:
    btpd_err("failed to initialize resume file (%s).\n", strerror(errno));
}

struct resume_data *
tlib_open_resume(struct tlib *tl, unsigned nfiles, size_t pfsize,
    size_t bfsize)
{
    int fd;
    char relpath[RELPATH_SIZE];
    struct stat sb;
    struct resume_data *resd = btpd_calloc(1, sizeof(*resd));
    bin2hex(tl->hash, relpath, 20);

    resd->size = 8 + nfiles * 16 + pfsize + bfsize;

    if ((errno =
            vopen(&fd, O_RDWR|O_CREAT, "torrents/%s/resume", relpath)) != 0)
        goto fatal;
    if (fstat(fd, &sb) != 0)
        goto fatal;
    if (sb.st_size != resd->size) {
        if (sb.st_size != 0 && ftruncate(fd, 0) != 0)
            goto fatal;
        init_resume(fd, resd->size);
    }
    resd->base =
        mmap(NULL, resd->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (resd->base == MAP_FAILED)
        goto fatal;
    if (bcmp(resd->base, "RESD", 4) != 0 || dec_be32(resd->base + 4) != 2)
        init_resume(fd, resd->size);
    close(fd);

    resd->pc_field = resd->base + 8 + nfiles * 16;
    resd->blk_field = resd->pc_field + pfsize;

    return resd;
fatal:
    btpd_err("file operation failed on 'torrents/%s/resume' (%s).\n",
        relpath, strerror(errno));
}

uint8_t *
resume_piece_field(struct resume_data *resd)
{
    return resd->pc_field;
}

uint8_t *
resume_block_field(struct resume_data *resd)
{
    return resd->blk_field;
}

void
resume_set_fts(struct resume_data *resd, int i, struct file_time_size *fts)
{
    enc_be64(resume_file_size(resd, i), (uint64_t)fts->size);
    enc_be64(resume_file_time(resd, i), (uint64_t)fts->mtime);
}

void
resume_get_fts(struct resume_data *resd, int i, struct file_time_size *fts)
{
    fts->size = dec_be64(resume_file_size(resd, i));
    fts->mtime = dec_be64(resume_file_time(resd, i));
}

void
tlib_close_resume(struct resume_data *resd)
{
    munmap(resd->base, resd->size);
    free(resd);
}
