#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "btpd.h"

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
    char cmd[PATH_MAX];
    assert(tl->tp == NULL);
    snprintf(cmd, PATH_MAX, "rm -r torrents/%s",
        bin2hex(tl->hash, relpath, 20));
    system(cmd);
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
    struct io_buffer iob = buf_init(1 << 10);

    buf_print(&iob,
        "d4:infod"
        "12:content havei%llde12:content sizei%llde"
        "3:dir%d:%s4:name%d:%s"
        "14:total downloadi%llde12:total uploadi%llde"
        "ee",
        tl->content_have, tl->content_size,
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
    buf_free(&iob);
    if (ferror(fp) || fclose(fp) != 0)
        btpd_err("failed to write '%s'.\n", wpath);
    if (rename(wpath, path) != 0)
        btpd_err("failed to rename: '%s' -> '%s' (%s).\n", wpath, path,
            strerror(errno));
}

void
tlib_update_info(struct tlib *tl)
{
    assert(tl->tp != NULL);
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
    return net_read32(k + 16);
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
tlib_load_resume(struct tlib *tl, unsigned nfiles, struct file_time_size *fts,
    size_t pfsize, uint8_t *pc_field, size_t bfsize, uint8_t *blk_field)
{
    int err, ver;
    FILE *fp;

    if ((err = vfopen(&fp, "r" , "torrents/%s/resume", tl->tp->relpath)) != 0)
        return err;

    if (fscanf(fp, "%d\n", &ver) != 1)
        goto invalid;
    if (ver != 1)
        goto invalid;
    for (int i = 0; i < nfiles; i++) {
        quad_t size;
        long time;
        if (fscanf(fp, "%qd %ld\n", &size, &time) != 2)
            goto invalid;
        fts[i].size = size;
        fts[i].mtime = time;
    }
    if (fread(pc_field, 1, pfsize, fp) != pfsize)
        goto invalid;
    if (fread(blk_field, 1, bfsize, fp) != bfsize)
        goto invalid;
    fclose(fp);
    return 0;
invalid:
    fclose(fp);
    bzero(pc_field, pfsize);
    bzero(blk_field, bfsize);
    return EINVAL;
}

void
tlib_save_resume(struct tlib *tl, unsigned nfiles, struct file_time_size *fts,
    size_t pfsize, uint8_t *pc_field, size_t bfsize, uint8_t *blk_field)
{
    int err;
    FILE *fp;
    if ((err = vfopen(&fp, "wb", "torrents/%s/resume", tl->tp->relpath)) != 0)
        return;
    fprintf(fp, "%d\n", 1);
    for (int i = 0; i < nfiles; i++)
        fprintf(fp, "%lld %ld\n", (long long)fts[i].size, (long)fts[i].mtime);
    fwrite(pc_field, 1, pfsize, fp);
    fwrite(blk_field, 1, bfsize, fp);
    if (fclose(fp) != 0); //XXX
}
