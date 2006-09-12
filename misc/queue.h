/*
 *      @(#)queue.h     8.5 (Berkeley) 8/20/94
 * $FreeBSD: src/sys/sys/queue.h,v 1.58.2.1 2005/01/31 23:26:57 imp Exp $
 */

#ifndef BTPD_QUEUE_H
#define BTPD_QUEUE_H

/*
 * Tail queue declarations.
 */
#define BTPDQ_HEAD(name, type)                                          \
struct name {                                                           \
        struct type *tqh_first; /* first element */                     \
        struct type **tqh_last; /* addr of last next element */         \
}

#define BTPDQ_HEAD_INITIALIZER(head)                                    \
        { NULL, &(head).tqh_first }

#define BTPDQ_ENTRY(type)                                               \
struct {                                                                \
        struct type *tqe_next;  /* next element */                      \
        struct type **tqe_prev; /* address of previous next element */  \
}

#define BTPDQ_EMPTY(head)       ((head)->tqh_first == NULL)

#define BTPDQ_FIRST(head)       ((head)->tqh_first)

#define BTPDQ_LAST(head, headname)                                      \
        (*(((struct headname *)((head)->tqh_last))->tqh_last))

#define BTPDQ_NEXT(elm, field) ((elm)->field.tqe_next)

#define BTPDQ_PREV(elm, headname, field)                                \
        (*(((struct headname *)((elm)->field.tqe_prev))->tqh_last))

#define BTPDQ_FOREACH(var, head, field)                                 \
        for ((var) = BTPDQ_FIRST((head));                               \
            (var);                                                      \
            (var) = BTPDQ_NEXT((var), field))

#define BTPDQ_FOREACH_MUTABLE(var, head, field, nvar)                   \
        for ((var) = BTPDQ_FIRST((head));                               \
             (var) && ((nvar) = BTPDQ_NEXT((var), field), (var));       \
             (var) = (nvar))

#define BTPDQ_INIT(head) do {                                           \
        BTPDQ_FIRST((head)) = NULL;                                     \
        (head)->tqh_last = &BTPDQ_FIRST((head));                        \
} while (0)

#define BTPDQ_INSERT_AFTER(head, listelm, elm, field) do {              \
        if ((BTPDQ_NEXT((elm), field) = BTPDQ_NEXT((listelm), field)) != NULL)\
                BTPDQ_NEXT((elm), field)->field.tqe_prev =              \
                    &BTPDQ_NEXT((elm), field);                          \
        else {                                                          \
                (head)->tqh_last = &BTPDQ_NEXT((elm), field);           \
        }                                                               \
        BTPDQ_NEXT((listelm), field) = (elm);                           \
        (elm)->field.tqe_prev = &BTPDQ_NEXT((listelm), field);          \
} while (0)

#define BTPDQ_INSERT_BEFORE(listelm, elm, field) do {                   \
        (elm)->field.tqe_prev = (listelm)->field.tqe_prev;              \
        BTPDQ_NEXT((elm), field) = (listelm);                           \
        *(listelm)->field.tqe_prev = (elm);                             \
        (listelm)->field.tqe_prev = &BTPDQ_NEXT((elm), field);          \
} while (0)

#define BTPDQ_INSERT_HEAD(head, elm, field) do {                        \
        if ((BTPDQ_NEXT((elm), field) = BTPDQ_FIRST((head))) != NULL)   \
                BTPDQ_FIRST((head))->field.tqe_prev =                   \
                    &BTPDQ_NEXT((elm), field);                          \
        else                                                            \
                (head)->tqh_last = &BTPDQ_NEXT((elm), field);           \
        BTPDQ_FIRST((head)) = (elm);                                    \
        (elm)->field.tqe_prev = &BTPDQ_FIRST((head));                   \
} while (0)

#define BTPDQ_INSERT_TAIL(head, elm, field) do {                        \
        BTPDQ_NEXT((elm), field) = NULL;                                \
        (elm)->field.tqe_prev = (head)->tqh_last;                       \
        *(head)->tqh_last = (elm);                                      \
        (head)->tqh_last = &BTPDQ_NEXT((elm), field);                   \
} while (0)

#define BTPDQ_REMOVE(head, elm, field) do {                             \
        if ((BTPDQ_NEXT((elm), field)) != NULL)                         \
                BTPDQ_NEXT((elm), field)->field.tqe_prev =              \
                    (elm)->field.tqe_prev;                              \
        else {                                                          \
                (head)->tqh_last = (elm)->field.tqe_prev;               \
        }                                                               \
        *(elm)->field.tqe_prev = BTPDQ_NEXT((elm), field);              \
} while (0)

#endif
