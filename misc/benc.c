#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
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
        assert((next = benc_mem(p, &blen, NULL)) != NULL);
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

const char *
benc_mem(const char *p, size_t *len, const char**next)
{
    if (!benc_isstr(p))
        return NULL;
    size_t blen = 0;
    blen = *p - '0';
    p++;
    while (isdigit(*p)) {
        blen *= 10;
        blen += *p - '0';
        p++;
    }
    assert(*p == ':');
    benc_safeset(len, blen);
    benc_safeset(next, *(p + blen + 1) == 'e' ? NULL : p + blen + 1);
    return p + 1;
}

char *
benc_str(const char *p, size_t *len, const char **next)
{
    size_t blen;
    const char *bstr;
    char *ret;
    if ((bstr = benc_mem(p, &blen, next)) == NULL)
        return NULL;
    if ((ret = malloc(blen + 1)) == NULL)
        return NULL;
    bcopy(bstr, ret, blen);
    ret[blen] = '\0';
    benc_safeset(len, blen);
    return ret;
}

char *
benc_mema(const char *p, size_t *len, const char **next)
{
    size_t blen;
    const char *bstr;
    char *ret;
    if ((bstr = benc_mem(p, &blen, next)) == NULL)
        return NULL;
    if ((ret = malloc(blen)) == NULL)
        return NULL;
    bcopy(bstr, ret, blen);
    benc_safeset(len, blen);
    return ret;
}

long long
benc_int(const char *p, const char **next)
{
    int sign = 1;
    long long res = 0;

    if (!benc_isint(p))
        return 0;
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
    benc_safeset(next, *(p + 1) == 'e' ? NULL : p + 1);    
    return res;
}

const char *
benc_dget_any(const char *p, const char *key)
{
    int cmp;
    size_t len, blen;
    const char *bstr;

    if (!benc_isdct(p))
        return NULL;

    len = strlen(key);

    p = benc_first(p);
    while (p != NULL) {
        if (!benc_isstr(p))
            return NULL;
        bstr = benc_mem(p, &blen, &p);

        cmp = strncmp(bstr, key, blen);
        if (cmp == 0 && len == blen)
            return p;
        else if (cmp <= 0)
            p = benc_next(p);
        else
            return NULL;
    }
    return NULL;
}

const char *
benc_dget_lst(const char *p, const char *key)
{
    const char *ret = benc_dget_any(p, key);
    return ret != NULL && benc_islst(ret) ? ret : NULL;
}

const char *
benc_dget_dct(const char *p, const char *key)
{
    const char *ret = benc_dget_any(p, key);
    return ret != NULL && benc_isdct(ret) ? ret : NULL;
}

const char *
benc_dget_mem(const char *p, const char *key, size_t *len)
{
    const char *str = benc_dget_any(p, key);
    return str != NULL && benc_isstr(str) ? benc_mem(str, len, NULL) : NULL;
}

char *
benc_dget_mema(const char *p, const char *key, size_t *len)
{
    const char *str = benc_dget_any(p, key);
    return str != NULL && benc_isstr(str) ? benc_mema(str, len, NULL) : NULL;
}

char *
benc_dget_str(const char *p, const char *key, size_t *len)
{
    const char *str = benc_dget_any(p, key);
    return str != NULL && benc_isstr(str) ? benc_str(str, len, NULL) : NULL;
}

long long
benc_dget_int(const char *p, const char *key)
{
    const char *intp = benc_dget_any(p, key);
    return intp != NULL && benc_isint(intp) ? benc_int(intp, NULL) : 0;
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

int
benc_istype(const char *p, enum be_type type)
{
    switch (type) {
    case BE_ANY:
        return benc_isdct(p) || benc_isint(p) ||
            benc_islst(p) || benc_isstr(p);
    case BE_DCT:
        return benc_isdct(p);
    case BE_INT:
        return benc_isint(p);
    case BE_LST:
        return benc_islst(p);
    case BE_STR:
        return benc_isstr(p);
    default:
        abort();
    }
}

int
benc_dct_chk(const char *p, int count, ...)
{
    int i, ok = 1;
    va_list ap;

    if (!benc_isdct(p))
        ok = 0;

    va_start(ap, count);
    for (i = 0; ok && i < count; i++) {
        enum be_type type = va_arg(ap, enum be_type);
        int level = va_arg(ap, int);
        const char *dct = p;
        const char *key = va_arg(ap, const char *);
        while (ok && level > 1) {            
            if ((dct = benc_dget_dct(dct, key)) != NULL) {
                level--;
                key = va_arg(ap, const char *);
            } else
                ok = 0;
        }
        if (ok) {
            const char *val = benc_dget_any(dct, key);
            if (val == NULL || !benc_istype(val, type))
                ok = 0;
        }
    }
    va_end(ap);
    return ok;
}

#if 0
int
benc_dct_type_check(const char *p, int count, ...)
{
    int i;
    va_list ap;

    benc_validate_dct(p, 2, BE_INT, "code", BE_STR, "hash", 

    if (!benc_isdct(p))
        return EINVAL;

    va_start(ap, count);
    for (i = 0; i < count; i++) {


    }
}

int
benc_dget_many(const char *p, int count, ...)
{
    int i;
    va_list ap;
    if (!benc_isdct(p))
        return 0;
    va_start(ap, count);
    for (i = 0; i < count; i++) {
        const char *name = va_arg(ap, const char *);
        enum be_type type = va_arg(ap, enum be_type);
        int64_t *iret;
        size_t *lret;
        const char **mret;
        char **aret;

        switch (type) {
        case BE_INT:
            iret = va_arg(ap, int64_t *);
            if (benc_dget_int64(p, name, iret) != 0)
                goto out;
            break;
        case BE_LST:
            mret = va_arg(ap, const char **);
            lret = va_arg(ap, size_t *);
            if (benc_dget_lst(p, name, mret) != 0)
                goto out;
            if (lret != NULL)
                *lret = benc_nelems(*mret);
            break;
        case BE_DCT:
            mret = va_arg(ap, const char **);
            if (benc_dget_dct(p, name, mret) != 0)
                goto out;
            break;
        case BE_MEM:
            mret = va_arg(ap, const char **);
            lret = va_arg(ap, size_t *);
            if (benc_dget_str(p, name, mret, lret) != 0)
                goto out;
            break;
        case BE_STRZ:
            aret = va_arg(ap, char **);
            lret = va_arg(ap, size_t *);
            if (benc_dget_strz(p, name, aret, lret) != 0)
                goto out;
            break;
        default:
            abort();
        }
    }
out:
    va_end(ap);
    return i;
}
#endif
