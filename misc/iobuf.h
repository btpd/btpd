#ifndef BTPD_IOBUF_H
#define BTPD_IOBUF_H

struct io_buffer {
    size_t buf_off;
    size_t buf_len;
    char *buf;
    int error;
};

int buf_init(struct io_buffer *iob, size_t size);
int buf_grow(struct io_buffer *iob, size_t size);
int buf_write(struct io_buffer *iob, const void *data, size_t size);
int buf_print(struct io_buffer *iob, const char *fmt, ...);

#define buf_swrite(iob, s) buf_write(iob, s, sizeof(s) - 1)

#endif
