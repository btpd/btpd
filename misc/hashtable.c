#include <stdlib.h>
#include <stdint.h>

#include "hashtable.h"

#define KEYP(tbl, o) ((o) + (tbl)->keyoff)
#define CHAINP(tbl, o) *((void **)((o) + (tbl)->chainoff))

struct _htbl {
    int (*eq)(const void *, const void *);
    uint32_t (*hash)(const void *);
    void **buckets;
    size_t buckcnt;
    size_t size;
    size_t keyoff;
    size_t chainoff;
};

struct _htbl *
_htbl_create(int (*eq)(const void *, const void *),
    uint32_t (*hash)(const void *), size_t keyoff, size_t chainoff)
{
    struct _htbl *tbl = calloc(1, sizeof(*tbl));
    if (tbl == NULL)
        return NULL;
    tbl->size = 0;
    tbl->buckcnt = 1;
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

static void *
bucket_rev(struct _htbl *tbl, void *p, void *n)
{
    while (n != NULL) {
        void *s = CHAINP(tbl, n);
        CHAINP(tbl, n) = p;
        p = n;
        n = s;
    }
    return p;
}

static void
bucket_insert(struct _htbl *tbl, void *o)
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
    void **obuckets = tbl->buckets;
    void **nbuckets = calloc(ncnt, sizeof(*nbuckets));
    if (nbuckets == NULL)
        return;

    tbl->buckcnt = ncnt;
    tbl->buckets = nbuckets;

    for (size_t i = 0; i < ocnt; i++) {
        void *o = bucket_rev(tbl, NULL, obuckets[i]);
        while (o != NULL) {
            void *s = CHAINP(tbl, o);
            bucket_insert(tbl, o);
            o = s;
        }
    }

    free(obuckets);
}

void
_htbl_insert(struct _htbl *tbl, void *o)
{
    bucket_insert(tbl, o);
    tbl->size++;
    if (tbl->size > tbl->buckcnt * 4 / 5)
        _htbl_grow(tbl);
}

void *
_htbl_find(struct _htbl *tbl, const void *key)
{
    size_t bi = tbl->hash(key) % tbl->buckcnt;
    for (void *ret = tbl->buckets[bi]; ret != NULL; ret = CHAINP(tbl, ret))
        if (tbl->eq(KEYP(tbl, ret), key))
            return ret;
    return NULL;
}

void *
_htbl_remove(struct _htbl *tbl, const void *key)
{
    size_t bi = tbl->hash(key) % tbl->buckcnt;
    void *p = NULL, *o = tbl->buckets[bi];
    while (o != NULL && !tbl->eq(KEYP(tbl, o), key)) {
        p = o;
        o = CHAINP(tbl, o);
    }
    if (o != NULL) {
        if (p == NULL)
            tbl->buckets[bi] = CHAINP(tbl, o);
        else
            CHAINP(tbl, p) = CHAINP(tbl, o);
        tbl->size--;
    }
    return o;
}

void
_htbl_tov(struct _htbl *tbl, void **v)
{
    size_t vi = 0;
    size_t bi = 0;
    void *o = tbl->buckets[bi];
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

size_t
_htbl_size(struct _htbl *tbl)
{
    return tbl->size;
}

void
_htbl_iter_init(struct _htbl *tbl, struct htbl_iter *it)
{
    it->tbl = tbl;
    it->bi = 0;
    it->cnt = 0;
    it->obj = NULL;
}

void *
_htbl_iter_next(struct htbl_iter *it)
{
    if (it->cnt == it->tbl->size)
        return NULL;
    it->obj = it->cnt == 0 ?
        it->tbl->buckets[it->bi] : CHAINP(it->tbl, it->obj);
    while (it->obj == NULL) {
        it->bi++;
        it->obj = it->tbl->buckets[it->bi];
    }
    it->cnt++;
    return it->obj;
}
