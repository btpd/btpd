#ifndef BTPD_HASHTABLE_H
#define BTPD_HASHTABLE_H

struct htbl_iter {
    struct _htbl *tbl;
    size_t bi;
    size_t cnt;
    struct _any *obj;
};

struct _htbl *_htbl_create(int (*equal)(const void *, const void *),
    uint32_t (*hash)(const void *), size_t keyoff, size_t chainoff);
void _htbl_free(struct _htbl *tbl);
void _htbl_insert(struct _htbl *tbl, struct _any *o);
struct _any *_htbl_remove(struct _htbl *tbl, const void *key);
struct _any *_htbl_find(struct _htbl *tbl, const void *key);
void _htbl_tov(struct _htbl *tb, struct _any **v);
size_t _htbl_size(struct _htbl *tbl);
void _htbl_iter_init(struct _htbl *tbl, struct htbl_iter *it);
struct _any *_htbl_iter_next(struct htbl_iter *it);

#define HTBL_ENTRY(name) struct _any *name

#define HTBL_TYPE(name, type, ktype, kname, cname) \
__attribute__((always_inline)) static inline struct name * \
name##_create(int (*equal)(const void *, const void *), \
    uint32_t (*hash)(const void *)) \
{ \
    return (struct name *) \
      _htbl_create(equal, hash, offsetof(struct type, kname), \
        offsetof(struct type, cname)); \
} \
\
__attribute__((always_inline)) static inline struct type * \
name##_find(struct name *tbl, const ktype *key) \
{ \
    return (struct type *)_htbl_find((struct _htbl *)tbl, key); \
} \
\
__attribute__((always_inline)) static inline struct type * \
name##_remove(struct name *tbl, const ktype *key) \
{ \
    return (struct type *)_htbl_remove((struct _htbl *)tbl, key); \
} \
\
__attribute__((always_inline)) static inline void \
name##_free(struct name *tbl) \
{ \
    _htbl_free((struct _htbl *)tbl); \
} \
\
__attribute__((always_inline)) static inline void \
name##_insert(struct name *tbl, struct type *o) \
{ \
    _htbl_insert((struct _htbl *)tbl, (struct _any *)o); \
} \
__attribute__((always_inline)) static inline void \
name##_tov(struct name *tbl, struct type **v) \
{ \
    _htbl_tov((struct _htbl *)tbl, (struct _any **)v); \
} \
\
__attribute__((always_inline)) static inline size_t \
name##_size(struct name *tbl) \
{ \
    return _htbl_size((struct _htbl *)tbl); \
} \
\
__attribute__((always_inline)) static inline void \
name##_iter_init(struct name *tbl, struct htbl_iter *it) \
{ \
    _htbl_iter_init((struct _htbl *)tbl, it); \
} \
\
__attribute__((always_inline)) static inline struct type * \
name##_iter_next(struct htbl_iter *it) \
{ \
    return (struct type *)_htbl_iter_next(it); \
}

#endif
