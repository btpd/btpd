#ifndef BTPD_BENC_H
#define BTPD_BENC_H

enum be_type {
    BE_ANY,
    BE_DCT,
    BE_INT,
    BE_LST,
    BE_STR
};

int benc_validate(const char *p, size_t len);
int benc_dct_chk(const char *p, int count, ...);

int benc_islst(const char *p);
int benc_isdct(const char *p);
int benc_isint(const char *p);
int benc_isstr(const char *p);

size_t benc_length(const char *p);
size_t benc_nelems(const char *p);

const char *benc_first(const char *p);
const char *benc_next(const char *p);

long long benc_int(const char *p, const char **next);
const char *benc_mem(const char *p, size_t *len, const char **next);
char *benc_mema(const char *p, size_t *len, const char **next);
char *benc_str(const char *p, size_t *len, const char **next);

const char *benc_dget_any(const char *p, const char *key);
const char *benc_dget_lst(const char *p, const char *key);
const char *benc_dget_dct(const char *p, const char *key);
long long benc_dget_int(const char *p, const char *key);
const char *benc_dget_mem(const char *p, const char *key, size_t *len);
char *benc_dget_mema(const char *p, const char *key, size_t *len);
char *benc_dget_str(const char *p, const char *key, size_t *len);

#endif
