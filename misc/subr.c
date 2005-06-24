#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
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
has_bit(uint8_t *bits, unsigned long index)
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
vopen(int *res, int flags, const char *fmt, ...)
{
    int fd, didmkdirs;
    char path[PATH_MAX + 1];
    va_list ap;

    va_start(ap, fmt);
    if (vsnprintf(path, PATH_MAX, fmt, ap) >= PATH_MAX) {
	va_end(ap);
	return ENAMETOOLONG;
    }
    va_end(ap);

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
canon_path(const char *path, char **res)
{
    char rp[PATH_MAX];

    if (realpath(path, rp) == NULL)
	return errno;
#if 0
    // This could be necessary on solaris.
    if (rp[0] != '/') {
	char wd[MAXPATHLEN];
	if (getcwd(wd, MAXPATHLEN) == NULL)
	    return errno;
	if (strlcat(wd, "/", MAXPATHLEN) >= MAXPATHLEN)
	    return ENAMETOOLONG;
	if (strlcat(wd, rp, MAXPATHLEN) >= MAXPATHLEN)
	    return ENAMETOOLONG;
	strcpy(rp, wd);
    }
#endif
    if ((*res = strdup(rp)) == NULL)
	return ENOMEM;

    return 0;
}

size_t
round_to_page(size_t size)
{
    size_t psize = getpagesize();
    size_t rem = size % psize;
    if (rem != 0)
	size += psize - rem;
    return size;
}
