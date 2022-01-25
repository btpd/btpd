#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "subr.h"

void *
memfind(const void *sub, size_t sublen, const void *mem, size_t memlen)
{
    size_t i, j;
    const uint8_t *s = sub, *m = mem;
    for (i = 0; i < memlen - sublen + 1; i++) {
        for (j = 0; j < sublen; j++)
            if (m[i+j] != s[j])
                break;
        if (j == sublen)
            return (void *)(m + i);
    }
    return NULL;
}

void
enc_be32(void *buf, uint32_t num)
{
    uint8_t *p = buf;
    *p = (num >> 24) & 0xff;
    *(p + 1) = (num >> 16) & 0xff;
    *(p + 2) = (num >> 8) & 0xff;
    *(p + 3) = num & 0xff;
}

uint32_t
dec_be32(const void *buf)
{
    const uint8_t *p = buf;
    return (uint32_t)*p << 24 | (uint32_t)*(p + 1) << 16
        | (uint16_t)*(p + 2) << 8 | *(p + 3);
}

void
enc_be64(void *buf, uint64_t num)
{
    uint8_t *p = buf;
    *p = (num >> 56) & 0xff;
    *(p + 1) = (num >> 48) & 0xff;
    *(p + 2) = (num >> 40) & 0xff;
    *(p + 3) = (num >> 32) & 0xff;
    *(p + 4) = (num >> 24) & 0xff;
    *(p + 5) = (num >> 16) & 0xff;
    *(p + 6) = (num >> 8) & 0xff;
    *(p + 7) = num & 0xff;
}

uint64_t
dec_be64(const void *buf)
{
    const uint8_t *p = buf;
    return (uint64_t)*p << 56 | (uint64_t)*(p + 1) << 48
        | (uint64_t)*(p + 2) << 40 | (uint64_t)*(p + 3) << 32
        | (uint64_t)*(p + 4) << 24 | (uint64_t)*(p + 5) << 16
        | (uint64_t)*(p + 6) << 8 | (uint64_t)*(p + 7);
}

void
set_bit(uint8_t *bits, unsigned long index)
{
    bits[index / 8] |= (1 << (7 - index % 8));
}

void
clear_bit(uint8_t *bits, unsigned long index)
{
    bits[index / 8] &= ~(1 << (7 - index % 8));
}

int
has_bit(const uint8_t *bits, unsigned long index)
{
    return bits[index / 8] & (1 << (7 - index % 8));
}

uint8_t
hex2i(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    else if (c >= 'a' && c <= 'f')
        return 10 + c - 'a';
    else
        abort();
}

int
ishex(char *str)
{
    while (*str != '\0') {
        if (!((*str >= '0' && *str <= '9') || (*str >= 'a' && *str <= 'f')))
            return 0;
        str++;
    }
    return 1;
}

uint8_t *
hex2bin(const char *hex, uint8_t *bin, size_t bsize)
{
    for (size_t i = 0; i < bsize; i++)
        bin[i] = hex2i(hex[i * 2]) << 4 | hex2i(hex[i * 2 + 1]);
    return bin;
}

char *
bin2hex(const uint8_t *bin, char *hex, size_t bsize)
{
    size_t i;
    const char *hexc = "0123456789abcdef";
    for (i = 0; i < bsize; i++) {
        hex[i * 2] = hexc[(bin[i] >> 4) & 0xf];
        hex[i * 2 + 1] = hexc[bin[i] &0xf];
    }
    hex[i * 2] = '\0';
    return hex;
}

int
set_nonblocking(int fd)
{
    int oflags;
    if ((oflags = fcntl(fd, F_GETFL, 0)) == -1)
        return errno;
    if (fcntl(fd, F_SETFL, oflags | O_NONBLOCK) == -1)
        return errno;
    return 0;
}

int
set_blocking(int fd)
{
    int oflags;
    if ((oflags = fcntl(fd, F_GETFL, 0)) == -1)
        return errno;
    if (fcntl(fd, F_SETFL, oflags & ~O_NONBLOCK) == -1)
        return errno;
    return 0;
}

int
mkdirs(char *path, int mode)
{
    int err = 0;
    char *spos = strchr(path + 1, '/'); // Skip leading '/'

    while (spos != NULL) {
        *spos = '\0';
        err = mkdir(path, mode);
        *spos = '/';

        if (err != 0 && errno != EEXIST)
            return errno;

        spos = strchr(spos + 1, '/');
    }
    if (mkdir(path, mode) != 0)
        return errno;
    return 0;
}

int
vaopen(int *res, int flags, const char *fmt, va_list ap)
{
    int fd, didmkdirs;
    char path[PATH_MAX + 1];

    if (vsnprintf(path, PATH_MAX, fmt, ap) >= PATH_MAX)
        return ENAMETOOLONG;

    didmkdirs = 0;
again:
    fd = open(path, flags, 0666);
    if (fd < 0 && errno == ENOENT && (flags & O_CREAT) != 0 && !didmkdirs) {
        char *rs = rindex(path, '/');
        if (rs != NULL) {
            *rs = '\0';
            if (mkdirs(path, 0777) == 0) {
                *rs = '/';
                didmkdirs = 1;
                goto again;
            }
        }
        return errno;
    }

    if (fd >= 0) {
        *res = fd;
        return 0;
    } else
        return errno;
}

int
vopen(int *res, int flags, const char *fmt, ...)
{
    int err;
    va_list ap;
    va_start(ap, fmt);
    err = vaopen(res, flags, fmt, ap);
    va_end(ap);
    return err;
}

int
vfsync(const char *fmt, ...)
{
    int err, fd;
    va_list ap;
    va_start(ap, fmt);
    err = vaopen(&fd, O_RDONLY, fmt, ap);
    va_end(ap);
    if (err != 0)
        return err;
    if (fsync(fd) < 0)
        err = errno;
    close(fd);
    return err;
}

int
vfopen(FILE **ret, const char *mode, const char *fmt, ...)
{
    int err = 0;
    char path[PATH_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    if (vsnprintf(path, PATH_MAX, fmt, ap) >= PATH_MAX)
        err = ENAMETOOLONG;
    va_end(ap);
    if (err == 0)
        if ((*ret = fopen(path, mode)) == NULL)
            err = errno;
    return err;
}

long
rand_between(long min, long max)
{
    return min + (long)rint((double)random() * (max - min) / RAND_MAX);
}

int
write_fully(int fd, const void *buf, size_t len)
{
    ssize_t nw;
    size_t off = 0;

    while (off < len) {
        nw = write(fd, buf + off, len - off);
        if (nw == -1)
            return errno;
        off += nw;
    }
    return 0;
}

int
read_fully(int fd, void *buf, size_t len)
{
    ssize_t nread;
    size_t off = 0;

    while (off < len) {
        nread = read(fd, buf + off, len - off);
        if (nread == 0)
            return EIO;
        else if (nread == -1)
            return errno;
        off += nread;
    }
    return 0;
}

void *
read_file(const char *path, void *buf, size_t *size)
{
    int fd, esave;
    void *mem = NULL;
    struct stat sb;

    if ((fd = open(path, O_RDONLY)) == -1)
        return NULL;
    if (fstat(fd, &sb) == -1)
        goto error;
    if (*size != 0 && *size < sb.st_size) {
        errno = EFBIG;
        goto error;
    }
    *size = sb.st_size;
    if (buf == NULL && (mem = malloc(sb.st_size)) == NULL)
        goto error;
    if (buf == NULL)
        buf = mem;
    if ((errno = read_fully(fd, buf, *size)) != 0)
        goto error;
    close(fd);
    return buf;

error:
    esave = errno;
    if (mem != NULL)
        free(mem);
    close(fd);
    errno = esave;
    return NULL;
}

char *
find_btpd_dir(void)
{
    char *xdg = getenv("XDG_DATA_HOME");
    if (xdg != NULL) {
        char *res;
        asprintf(&res, "%s/btpd", xdg);
        return res;
    }
    char *res = getenv("BTPD_HOME");
    if (res != NULL)
        return strdup(res);
    char *home = getenv("HOME");
    if (home == NULL) {
        struct passwd *pwent = getpwuid(getuid());
        endpwent();
        if (pwent != NULL)
            home = pwent->pw_dir;
    }
    if (home != NULL)
        asprintf(&res, "%s/.btpd", home);
    return res;
}

int
make_abs_path(const char *in, char *out)
{
    int ii = 0, oi = 0, lastsep = 0;
    switch (in[0]) {
    case '\0':
        return EINVAL;
    case '/':
        if (strlen(in) >= PATH_MAX)
            return ENAMETOOLONG;
        out[0] = '/';
        oi++;
        ii++;
        break;
    default:
        if (getcwd(out, PATH_MAX) == NULL)
            return errno;
        oi = strlen(out);
        if (oi + strlen(in) + 1 >= PATH_MAX)
            return ENAMETOOLONG;
        out[oi] = '/';
        lastsep = oi;
        oi++;
        break;
    }
after_slash:
    while (in[ii] == '/')
        ii++;
    switch(in[ii]) {
    case '\0':
        goto end;
    case '.':
        ii++;
        goto one_dot;
    default:
        goto normal;
    }
one_dot:
    switch (in[ii]) {
    case '\0':
        goto end;
    case '/':
        ii++;
        goto after_slash;
    case '.':
        ii++;
        goto two_dot;
    default:
        out[oi] = '.';
        oi++;
        goto normal;
    }
two_dot:
    switch (in[ii]) {
    case '\0':
        if (lastsep == 0)
            oi = 1;
        else {
            oi = lastsep;
            while (out[oi - 1] != '/')
                oi--;
            lastsep = oi - 1;
        }
        goto end;
    case '/':
        if (lastsep == 0)
            oi = 1;
        else {
            oi = lastsep;
            while (out[oi - 1] != '/')
                oi--;
            lastsep = oi - 1;
        }
        ii++;
        goto after_slash;
    default:
        out[oi] = '.';
        out[oi + 1] = '.';
        oi += 2;
        goto normal;
    }
normal:
    switch (in[ii]) {
    case '\0':
        goto end;
    case '/':
        out[oi] = '/';
        lastsep = oi;
        oi++;
        ii++;
        goto after_slash;
    default:
        out[oi] = in[ii];
        oi++;
        ii++;
        goto normal;
    }
end:
    if (oi == lastsep + 1 && lastsep != 0)
        oi = lastsep;
    out[oi] = '\0';
    return 0;
}

#ifndef HAVE_ASPRINTF
int
asprintf(char **strp, const char *fmt, ...)
{
    int np;
    va_list ap;
    va_start(ap, fmt);
    np = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if ((*strp = malloc(np + 1)) == NULL)
        return -1;
    va_start(ap, fmt);
    vsnprintf(*strp, np + 1, fmt, ap);
    va_end(ap);
    return np;
}
#endif
