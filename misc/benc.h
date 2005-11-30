#ifndef BTPD_BENC_H
#define BTPD_BENC_H

int benc_validate(const char *p, size_t len);

size_t benc_length(const char *p);
size_t benc_nelems(const char *p);

const char *benc_first(const char *p);
const char *benc_next(const char *p);

int benc_str(const char *p, const char **mem, size_t *len, const char**next);
int benc_stra(const char *p, char **out, size_t *len, const char **next);
int benc_strz(const char *p, char **out, size_t *len, const char **next);
int benc_int64(const char *p, int64_t *out, const char **next);
int benc_uint32(const char *p, uint32_t *out, const char **next);

#define benc_off benc_int64

int benc_dget_any(const char *p, const char *key, const char **val);
int benc_dget_lst(const char *p, const char *key, const char **val);
int benc_dget_dct(const char *p, const char *key, const char **val);
int benc_dget_str(const char *p, const char *key,
                  const char **val, size_t *len);
int benc_dget_stra(const char *p, const char *key, char **val, size_t *len);
int benc_dget_strz(const char *p, const char *key, char **val, size_t *len);
int benc_dget_int64(const char *p, const char *key, int64_t *val);
int benc_dget_uint32(const char *p, const char *key, uint32_t *val);

#define benc_dget_off benc_dget_int64

int benc_islst(const char *p);
int benc_isdct(const char *p);
int benc_isint(const char *p);
int benc_isstr(const char *p);

#endif
