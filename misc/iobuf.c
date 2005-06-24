#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iobuf.h"

#define GROWLEN (1 << 14)

int
buf_init(struct io_buffer *iob, size_t size)
{
    iob->buf_off = 0;
    iob->buf_len = size;
    iob->buf = malloc(size);
    if (iob->buf == NULL)
        return ENOMEM;
    else
        return 0;
}

int
buf_grow(struct io_buffer *iob, size_t addlen)
{
    char *nbuf = realloc(iob->buf, iob->buf_len + addlen);
    if (nbuf == NULL)
        return ENOMEM;
    else {
        iob->buf = nbuf;
        iob->buf_len += addlen;
        return 0;
    }
}

int
buf_print(struct io_buffer *iob, const char *fmt, ...)
{
    int np;
    va_list ap;
    va_start(ap, fmt);
    np = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    while (np + 1 > iob->buf_len - iob->buf_off)
        if (buf_grow(iob, GROWLEN) != 0)
            return ENOMEM;
    va_start(ap, fmt);
    vsnprintf(iob->buf + iob->buf_off, np + 1, fmt, ap);
    va_end(ap);
    iob->buf_off += np;
    return 0;
}

int
buf_write(struct io_buffer *iob, const void *buf, size_t len)
{
    while (iob->buf_len - iob->buf_off < len)
        if (buf_grow(iob, GROWLEN) != 0)
            return ENOMEM;
    bcopy(buf, iob->buf + iob->buf_off, len);
    iob->buf_off += len;
    return 0;
}
