#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "benc.h"

#define benc_safeset(out, val) if ((out) != NULL) *(out) = (val)

static const char *benc_validate_aux(const char *p, const char *end);

int
benc_validate(const char *p, size_t len)
{
    const char *end = p + len - 1;

    if (len <= 0)
        return EINVAL;

    return benc_validate_aux(p, end) == end ? 0 : EINVAL;
}

static const char *
benc_validate_aux(const char *p, const char *end)
{
    size_t d = 0;
    switch (*p) {
    case 'd':
        d = 1;
    case 'l':
        for (p++; p <= end && *p != 'e'; p++) {
            if (d != 0) {
                if (d % 2 == 1 && !isdigit(*p))
                    return NULL;
                else
                    d++;
            }
            if ((p = benc_validate_aux(p, end)) == NULL)
                return NULL;
        }
        if (p > end || (d != 0 && d % 2 != 1))
            return NULL;
        break;
    case 'i':
        p++;
        if (p > end)
            return NULL;
        if (*p == '-')
            p++;
        if (p > end || !isdigit(*p))
            return NULL;
        p++;
        while (p <= end && isdigit(*p))
            p++;
        if (p > end || *p != 'e')
            return NULL;
        break;
    default:
        if (isdigit(*p)) {
            size_t len = 0;
            while (p <= end && isdigit(*p)) {
                len *= 10;
                len += *p - '0';
                p++;
            }
            if (p <= end && *p == ':' && p + len <= end)
                p += len;
            else
                return NULL;
        }
        else
            return NULL;
        break;
    }
    return p;
}

size_t
benc_length(const char *p)
{
    size_t blen;
    const char *next;

    switch (*p) {
    case 'd':
    case 'l':
        blen = 2; // [l|d]...e
        next = benc_first(p);
        while (*next != 'e') {
            size_t len = benc_length(next);
            blen += len;
            next += len;
        }
        return blen;
    case 'i':
        for (next = p + 1; *next != 'e'; next++)
            ;
        return next - p + 1;
    default:
        assert(benc_str(p, &next, &blen, NULL) == 0);
        return next - p + blen;
    }
}

size_t
benc_nelems(const char *p)
{
    size_t nelems = 0;
    for (p = benc_first(p); p != NULL; p = benc_next(p))
        nelems++;
    return nelems;
}

const char *
benc_first(const char *p)
{
    assert(benc_islst(p));
    return *(p + 1) == 'e' ? NULL : p + 1;
}

const char *
benc_next(const char *p)
{
    size_t blen = benc_length(p);
    return *(p + blen) == 'e' ? NULL : p + blen;
}

int
benc_str(const char *p, const char **out, size_t *len, const char**next)
{
    size_t blen = 0;
    assert(isdigit(*p));
    blen = *p - '0';
    p++;
    while (isdigit(*p)) {
        blen *= 10;
        blen += *p - '0';
        p++;
    }
    assert(*p == ':');
    benc_safeset(len, blen);
    benc_safeset(out, p + 1);
    benc_safeset(next, *(p + blen + 1) == 'e' ? NULL : p + blen + 1);
    return 0;
}

int
benc_strz(const char *p, char **out, size_t *len, const char **next)
{
    int err;
    size_t blen;
    const char *bstr;

    if ((err = benc_str(p, &bstr, &blen, next)) == 0) {
        if ((*out = malloc(blen + 1)) != NULL) {
            memcpy(*out, bstr, blen);
            (*out)[blen] = '\0';
            benc_safeset(len, blen);
        } else
            err = ENOMEM;
    }
    return err;
}

int
benc_stra(const char *p, char **out, size_t *len, const char **next)
{
    int err;
    size_t blen;
    const char *bstr;

    if ((err = benc_str(p, &bstr, &blen, next)) == 0) {
        if ((*out = malloc(blen)) != NULL) {
            memcpy(*out, bstr, blen);
            benc_safeset(len, blen);
        } else
            err = ENOMEM;
    }
    return err;
}

int
benc_int64(const char *p, int64_t *out, const char **next)
{
    int sign = 1;
    int64_t res = 0;

    assert(*p == 'i');
    p++;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    assert(isdigit(*p));
    res += sign * (*p - '0');
    p++;
    while (isdigit(*p)) {
        res *= sign * 10;
        res += sign * (*p - '0');
        p++;
    }
    assert(*p == 'e');
    benc_safeset(out, res);
    benc_safeset(next, *(p + 1) == 'e' ? NULL : p + 1);

    return 0;
}

int
benc_uint32(const char *p, uint32_t *out, const char **next)
{
    int err;
    int64_t res;
    if ((err = benc_int64(p, &res, next)) == 0) {
        if (res >= 0 && res <= 0xffffffffUL)
            *out = (uint32_t)res;
        else
            err = EINVAL;
    }
    return err;
}

int
benc_dget_any(const char *p, const char *key, const char **val)
{
    int res;
    size_t len, blen;
    const char *bstr;

    assert(benc_isdct(p));

    len = strlen(key);

    p = benc_first(p);
    while (p != NULL) {
        if ((res = benc_str(p, &bstr, &blen, &p)) != 0)
            return res;

        res = strncmp(bstr, key, blen);
        if (res == 0 && len == blen) {
            *val = p;
            return 0;
        } else if (res <= 0) {
            p = benc_next(p);
        } else
            return ENOENT;
    }
    return ENOENT;
}

int
benc_dget_lst(const char *p, const char *key, const char **val)
{
    int err;
    if ((err = benc_dget_any(p, key, val)) == 0)
        if (!benc_islst(*val))
            err = EINVAL;
    return err;
}

int
benc_dget_dct(const char *p, const char *key, const char **val)
{
    int err;
    if ((err = benc_dget_any(p, key, val)) == 0)
        if (!benc_isdct(*val))
            err = EINVAL;
    return err;
}

int
benc_dget_str(const char *p, const char *key, const char **val, size_t *len)
{
    int err;
    const char *sp;
    if ((err = benc_dget_any(p, key, &sp)) == 0)
        err = benc_isstr(sp) ? benc_str(sp, val, len, NULL) : EINVAL;
    return err;
}

int
benc_dget_stra(const char *p, const char *key, char **val, size_t *len)
{
    int err;
    const char *sp;
    if ((err = benc_dget_any(p, key, &sp)) == 0)
        err = benc_isstr(sp) ? benc_stra(sp, val, len, NULL) : EINVAL;
    return err;
}

int
benc_dget_strz(const char *p, const char *key, char **val, size_t *len)
{
    int err;
    const char *sp;
    if ((err = benc_dget_any(p, key, &sp)) == 0)
        err = benc_isstr(sp) ? benc_strz(sp, val, len, NULL) : EINVAL;
    return err;
}

int
benc_dget_int64(const char *p, const char *key, int64_t *val)
{
    int err;
    const char *ip;
    if ((err = benc_dget_any(p, key, &ip)) == 0)
        err = benc_isint(ip) ? benc_int64(ip, val, NULL) : EINVAL;
    return err;
}

int
benc_dget_uint32(const char *p, const char *key, uint32_t *val)
{
    int err;
    const char *ip;
    if ((err = benc_dget_any(p, key, &ip)) == 0)
        err = benc_isint(ip) ? benc_uint32(ip, val, NULL) : EINVAL;
    return err;
}

int
benc_islst(const char *p)
{
    return *p == 'l' || *p == 'd';
}

int
benc_isdct(const char *p)
{
    return *p == 'd';
}

int
benc_isint(const char *p)
{
    return *p == 'i';
}

int
benc_isstr(const char *p)
{
    return isdigit(*p);
}
