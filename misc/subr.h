#ifndef BTPD_SUBR_H
#define BTPD_SUBR_H

#define min(x, y) ((x) <= (y) ? (x) : (y))

int set_nonblocking(int fd);
int set_blocking(int fd);

int mkdirs(char *path);

int vopen(int *resfd, int flags, const char *fmt, ...);

void set_bit(uint8_t *bits, unsigned long index);
int has_bit(uint8_t *bits, unsigned long index);
void clear_bit(uint8_t *bits, unsigned long index);

int canon_path(const char *path, char **res);

size_t round_to_page(size_t size);

#endif
