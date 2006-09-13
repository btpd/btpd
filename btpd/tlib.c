#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
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

static int
valid_info(char *buf, size_t len)
{
    size_t slen;
    if (benc_validate(buf, len) != 0)
        return 0;
    if (benc_dget_mem(buf, "name", &slen) == NULL || slen == 0)
        return 0;
    if ((benc_dget_mem(buf, "dir", &slen) == NULL ||
            (slen == 0 || slen >= PATH_MAX)))
        return 0;
    return 1;
}

static void
load_info(struct tlib *tl, const char *path)
{
    size_t size = 1 << 14;
    char buf[size], *p = buf;

    if ((errno = read_whole_file((void **)&p, &size, path)) != 0) {
        btpd_log(BTPD_L_ERROR, "couldn't load '%s' (%s).\n", path,
            strerror(errno));
        return;
    }

    if (!valid_info(buf, size)) {
        btpd_log(BTPD_L_ERROR, "bad info file '%s'.\n", path);
        return ;
    }

    tl->name = benc_dget_str(buf, "name", NULL);
    tl->dir = benc_dget_str(buf, "dir", NULL);
#if 0
    tl->t_added = benc_dget_int(buf, "time added");
    tl->t_active = benc_dget_int(buf, "time active");
    tl->tot_up = benc_dget_int(buf, "total upload");
    tl->tot_down = benc_dget_int(buf, "total download");
#endif
    if (tl->name == NULL || tl->dir == NULL)
        btpd_err("out of memory.\n");
}

static void
save_info(struct tlib *tl, const char *path)
{
    FILE *fp;
    char wpath[PATH_MAX];
    snprintf(wpath, PATH_MAX, "%s.write", path);
    if ((fp = fopen(wpath, "w")) == NULL)
        btpd_err("failed to open '%s' (%s).\n", wpath, strerror(errno));
    fprintf(fp, "d3:dir%d:%s4:name%d:%s", (int)strlen(tl->dir), tl->dir,
        (int)strlen(tl->name), tl->name);
#if 0
    fprintf(fp, "11:time activei%lde10:time addedi%lde", tl->t_active,
        tl->t_added);
    fprintf(fp, "14:total downloadi%llde12:total uploadi%lldee", tl->tot_down,
        tl->tot_up);
#else
    fprintf(fp, "e");
#endif
    if (ferror(fp) || fclose(fp) != 0)
        btpd_err("failed to write '%s'.\n", wpath);
    if (rename(wpath, path) != 0)
        btpd_err("failed to rename: '%s' -> '%s' (%s).\n", wpath, path,
            strerror(errno));
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
#if 0
    struct timeval tv;
#endif
    char relpath[RELPATH_SIZE], file[PATH_MAX];
    bin2hex(hash, relpath, 20);

    if (name == NULL)
        if ((name = mi_name(mi)) == NULL)
            btpd_err("out of memory.\n");

    tl->name = name;
    tl->dir = strdup(content);
    if (tl->name == NULL || tl->dir == NULL)
        btpd_err("out of memory.\n");

#if 0
    gettimeofday(&tv, NULL);
    tl->t_added = tv.tv_sec;
#endif

    snprintf(file, PATH_MAX, "torrents/%s", relpath);
    if (mkdir(file, 0777) != 0)
        btpd_err("failed to create dir '%s' (%s).\n", file, strerror(errno));
    snprintf(file, PATH_MAX, "torrents/%s/torrent", relpath);
    write_torrent(mi, mi_size, file);
    snprintf(file, PATH_MAX, "torrents/%s/info", relpath);
    save_info(tl, file);
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
        if (dp->d_namlen == 40 && ishex(dp->d_name)) {
            struct tlib * tl = tlib_create(hex2bin(dp->d_name, hash, 20));
            snprintf(file, PATH_MAX, "torrents/%s/info", dp->d_name);
            load_info(tl, file);
        }
    }
    closedir(dirp);
}
