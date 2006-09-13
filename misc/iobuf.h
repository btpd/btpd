#ifndef BTPD_IOBUF_H
#define BTPD_IOBUF_H

struct io_buffer {
    size_t off;
    size_t len;
    char *buf;
    int error;
};

struct io_buffer buf_init(size_t size);
void buf_free(struct io_buffer *iob);
int buf_grow(struct io_buffer *iob, size_t size);
int buf_write(struct io_buffer *iob, const void *data, size_t size);
__attribute__((format (printf, 2, 3)))
int buf_print(struct io_buffer *iob, const char *fmt, ...);

#define buf_swrite(iob, s) buf_write(iob, s, sizeof(s) - 1)

#endif
