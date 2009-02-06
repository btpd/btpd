#include <stdlib.h>
#include <inttypes.h>

#include "hashtable.h"

struct _htbl {
    int (*eq)(const void *, const void *);
    uint32_t (*hash)(const void *);
    struct _any **buckets;
    size_t buckcnt;
    size_t size;
    size_t keyoff;
    size_t chainoff;
    float ratio;
};

#define KEYP(tbl, o) ((void *)(o) + (tbl)->keyoff)
#define CHAINP(tbl, o) *(struct _any **)((void *)(o) + (tbl)->chainoff)

struct _htbl *
_htbl_create(float ratio, int (*eq)(const void *, const void *),
    uint32_t (*hash)(const void *), size_t keyoff, size_t chainoff)
{
    struct _htbl *tbl = calloc(1, sizeof(*tbl));
    if (tbl == NULL)
        return NULL;
    tbl->size = 0;
    tbl->buckcnt = 1;
    tbl->ratio = ratio;
    tbl->keyoff = keyoff;
    tbl->chainoff = chainoff;
    tbl->hash = hash;
    tbl->eq = eq;
    tbl->buckets = calloc(tbl->buckcnt, sizeof(*tbl->buckets));
    if (tbl->buckets == NULL) {
        free(tbl);
        return NULL;
    }
    return tbl;
}

void
_htbl_free(struct _htbl *tbl)
{
    free(tbl->buckets);
    free(tbl);
}

static struct _any *
bucket_rev(struct _htbl *tbl, struct _any *p, struct _any *n)
{
    while (n != NULL) {
        struct _any *s = CHAINP(tbl, n);
        CHAINP(tbl, n) = p;
        p = n;
        n = s;
    }
    return p;
}

static void
bucket_insert(struct _htbl *tbl, struct _any *o)
{
    size_t bi = tbl->hash(KEYP(tbl, o)) % tbl->buckcnt;
    CHAINP(tbl, o) = tbl->buckets[bi];
    tbl->buckets[bi] = o;
}

static void
_htbl_grow(struct _htbl *tbl)
{
    size_t ncnt = 2 * tbl->buckcnt + 1;
    size_t ocnt = tbl->buckcnt;
    struct _any **obuckets = tbl->buckets;
    struct _any **nbuckets = calloc(ncnt, sizeof(*nbuckets));
    if (nbuckets == NULL)
        return;

    tbl->buckcnt = ncnt;
    tbl->buckets = nbuckets;

    for (size_t i = 0; i < ocnt; i++) {
        struct _any *o = bucket_rev(tbl, NULL, obuckets[i]);
        while (o != NULL) {
            struct _any *s = CHAINP(tbl, o);
            bucket_insert(tbl, o);
            o = s;
        }
    }

    free(obuckets);
}

void
_htbl_insert(struct _htbl *tbl, struct _any *o)
{
    bucket_insert(tbl, o);
    tbl->size++;
    if (tbl->size > tbl->buckcnt * tbl->ratio)
        _htbl_grow(tbl);
}

struct _any *
_htbl_find(struct _htbl *tbl, const void *key)
{
    struct _any *ret;
    size_t bi = tbl->hash(key) % tbl->buckcnt;
    for (ret = tbl->buckets[bi]; ret != NULL; ret = CHAINP(tbl, ret))
        if (tbl->eq(KEYP(tbl, ret), key))
            return ret;
    return NULL;
}

struct _any *
_htbl_remove(struct _htbl *tbl, const void *key)
{
    size_t bi = tbl->hash(key) % tbl->buckcnt;
    struct _any **p = &tbl->buckets[bi], *o = tbl->buckets[bi];
    while (o != NULL && !tbl->eq(KEYP(tbl, o), key)) {
        p = &CHAINP(tbl, o);
        o = *p;
    }
    if (o != NULL) {
        *p = CHAINP(tbl, o);
        tbl->size--;
    }
    return o;
}

void
_htbl_fillv(struct _htbl *tbl, struct _any **v)
{
    size_t vi = 0;
    size_t bi = 0;
    struct _any *o = tbl->buckets[bi];
    while (vi < tbl->size) {
        while (o == NULL) {
            bi++;
            o = tbl->buckets[bi];
        }
        v[vi] = o;
        vi++;
        o = CHAINP(tbl, o);
    }
}

struct _any **
_htbl_tov(struct _htbl *tbl)
{
    struct _any **v = malloc(sizeof(*v));
    if (v != NULL)
        _htbl_fillv(tbl, v);
    return v;
}

size_t
_htbl_size(struct _htbl *tbl)
{
    return tbl->size;
}

static void
iter_next_bucket(struct htbl_iter *it)
{
    while (it->tbl->buckets[it->bi] == NULL)
        it->bi++;
    it->obj = it->tbl->buckets[it->bi];
    it->ptr = &it->tbl->buckets[it->bi];
}

struct _any *
_htbl_iter_first(struct _htbl *tbl, struct htbl_iter *it)
{
    if (tbl->size == 0)
        return NULL;

    it->tbl = tbl;
    it->cnt = 1;
    it->bi = 0;
    iter_next_bucket(it);
    return it->obj;
}

struct _any *
_htbl_iter_next(struct htbl_iter *it)
{
    struct _any *tmp;
    if (it->cnt == it->tbl->size)
        return NULL;

    it->cnt++;
    if ((tmp = CHAINP(it->tbl, it->obj)) != NULL) {
        it->ptr = &CHAINP(it->tbl, it->obj);
        it->obj = tmp;
    } else {
        it->bi++;
        iter_next_bucket(it);
    }
    return it->obj;
}

#include <stdio.h>

struct _any *
_htbl_iter_del(struct htbl_iter *it)
{
    struct _any *tmp;

    it->tbl->size--;
    if (it->cnt > it->tbl->size) {
        *it->ptr = NULL;
        return NULL;
    }

    if ((tmp = CHAINP(it->tbl, it->obj)) != NULL) {
        *it->ptr = tmp;
        it->obj = tmp;
    } else {
        *it->ptr = NULL;
        it->bi++;
        iter_next_bucket(it);
    }
    return it->obj;
}
