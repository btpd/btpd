#ifndef BTPD_HASHTABLE_H
#define BTPD_HASHTABLE_H

struct htbl_iter {
    struct _htbl *tbl;
    size_t bi;
    size_t cnt;
    void *obj;
};

struct _htbl *_htbl_create(int (*equal)(const void *, const void *),
    uint32_t (*hash)(const void *), size_t keyoff, size_t chainoff);
void _htbl_free(struct _htbl *tbl);
void _htbl_insert(struct _htbl *tbl, void *o);
void *_htbl_remove(struct _htbl *tbl, const void *key);
void *_htbl_find(struct _htbl *tbl, const void *key);
void _htbl_tov(struct _htbl *tb, void **v);
size_t _htbl_size(struct _htbl *tbl);
void _htbl_iter_init(struct _htbl *tbl, struct htbl_iter *it);
void *_htbl_iter_next(struct htbl_iter *it);

#define HTBLTYPE(name, type, ktype, kname, cname) \
__attribute__((always_inline)) static inline struct name * \
name##_create(int (*equal)(const ktype *, const ktype *), \
    uint32_t (*hash)(const ktype *)) \
{ \
    return (struct name *) \
      _htbl_create((int (*)(const void *, const void *))equal, \
        (uint32_t (*)(const void *))hash, offsetof(struct type, kname), \
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
    _htbl_insert((struct _htbl *)tbl, o); \
} \
__attribute__((always_inline)) static inline void \
name##_tov(struct name *tbl, struct type **v) \
{ \
    _htbl_tov((struct _htbl *)tbl, (void **)v); \
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
