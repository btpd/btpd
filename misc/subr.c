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
            return ECONNRESET;
        else if (nread == -1)
            return errno;
        off += nread;
    }
    return 0;
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
