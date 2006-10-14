#ifndef BTPD_SUBR_H
#define BTPD_SUBR_H

#include <stdio.h>
#include <stdarg.h>

#define min(x, y) ((x) <= (y) ? (x) : (y))
#define SHAHEXSIZE 41

int set_nonblocking(int fd);
int set_blocking(int fd);

int mkdirs(char *path);

__attribute__((format (printf, 3, 0)))
int vaopen(int *resfd, int flags, const char *fmt, va_list ap);
__attribute__((format (printf, 3, 4)))
int vopen(int *resfd, int flags, const char *fmt, ...);
__attribute__((format (printf, 3, 4)))
int vfopen(FILE **ret, const char *mode, const char *fmt, ...);
int vfsync(const char *fmt, ...);

void set_bit(uint8_t *bits, unsigned long index);
int has_bit(const uint8_t *bits, unsigned long index);
void clear_bit(uint8_t *bits, unsigned long index);

char *bin2hex(const uint8_t *bin, char *hex, size_t bsize);
uint8_t *hex2bin(const char *hex, uint8_t *bin, size_t bsize);
uint8_t hex2i(char c);
int ishex(char *str);

long rand_between(long min, long max);

int read_fully(int fd, void *buf, size_t len);
int write_fully(int fd, const void *buf, size_t len);
void *read_file(const char *path, void *buf, size_t *size);

char *find_btpd_dir(void);

#endif
