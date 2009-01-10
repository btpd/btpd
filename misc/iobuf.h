#ifndef BTPD_IOBUF_H
#define BTPD_IOBUF_H

struct iobuf {
    uint8_t *buf;
    size_t size;
    size_t off;
    size_t skip;
    int error;
};

struct iobuf iobuf_init(size_t size);
void iobuf_free(struct iobuf *iob);
int iobuf_accommodate(struct iobuf *iob, size_t size);
int iobuf_write(struct iobuf *iob, const void *data, size_t size);
__attribute__((format (printf, 2, 3)))
int iobuf_print(struct iobuf *iob, const char *fmt, ...);
void *iobuf_find(struct iobuf *iob, const void *p, size_t plen);
void iobuf_consumed(struct iobuf *iob, size_t count);

#define iobuf_swrite(iob, s) iobuf_write(iob, s, sizeof(s) - 1)

#endif
