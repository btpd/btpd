#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iobuf.h"

#define GROWLEN (1 << 14)

struct io_buffer
buf_init(size_t size)
{
    struct io_buffer iob;
    iob.off = 0;
    iob.len = size;
    iob.error = (iob.buf = malloc(size)) == NULL ? 1 : 0;
    return iob;
}

void
buf_free(struct io_buffer *iob)
{
    if (iob->buf != NULL)
        free(iob->buf);
}

int
buf_grow(struct io_buffer *iob, size_t addlen)
{
    if (iob->error)
        return iob->error;
    char *nbuf = realloc(iob->buf, iob->len + addlen);
    if (nbuf == NULL) {
        iob->error = 1;
        return 0;
    } else {
        iob->buf = nbuf;
        iob->len += addlen;
        return 1;
    }
}

int
buf_print(struct io_buffer *iob, const char *fmt, ...)
{
    if (iob->error)
        return 0;
    int np;
    va_list ap;
    va_start(ap, fmt);
    np = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (np + 1 > iob->len - iob->off)
        if (!buf_grow(iob, (1 + (np + 1) / GROWLEN) * GROWLEN))
            return 0;
    va_start(ap, fmt);
    vsnprintf(iob->buf + iob->off, np + 1, fmt, ap);
    va_end(ap);
    iob->off += np;
    return 1;
}

int
buf_write(struct io_buffer *iob, const void *buf, size_t len)
{
    if (iob->error)
        return 0;
    if (len > iob->len - iob->off)
        if (!buf_grow(iob, (1 + len / GROWLEN) * GROWLEN))
            return 0;
    bcopy(buf, iob->buf + iob->off, len);
    iob->off += len;
    return 1;
}
