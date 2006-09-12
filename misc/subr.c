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
#include <unistd.h>

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
mkdirs(char *path)
{
    int err = 0;
    char *spos = strchr(path + 1, '/'); // Must ignore the root

    while (spos != NULL) {
        *spos = '\0';
        err = mkdir(path, 0777);
        *spos = '/';

        if (err != 0 && errno != EEXIST) {
            err = errno;
            break;
        }

        spos = strchr(spos + 1, '/');
    }
    return err;
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
        if (mkdirs(path) == 0) {
            didmkdirs = 1;
            goto again;
        } else
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

int
read_whole_file(void **out, size_t *size, const char *fmt, ...)
{
    int err, fd;
    int didmalloc = 0;
    struct stat sb;
    va_list ap;

    va_start(ap, fmt);
    err = vaopen(&fd, O_RDONLY, fmt, ap);
    va_end(ap);
    if (err != 0)
        return err;
    if (fstat(fd, &sb) != 0) {
        err = errno;
        goto error;
    }
    if (*size != 0 && *size < sb.st_size) {
        err = EFBIG;
        goto error;
    }
    *size = sb.st_size;
    if (*out == NULL) {
        if ((*out = malloc(*size)) == NULL) {
            err = errno;
            goto error;
        }
        didmalloc = 1;
    }
    if ((err = read_fully(fd, *out, *size)) != 0)
        goto error;

    close(fd);
    return 0;

error:
    if (didmalloc)
        free(*out);
    close(fd);
    return err;
}

char *
find_btpd_dir(void)
{
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
