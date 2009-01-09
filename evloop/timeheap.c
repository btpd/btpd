#include <sys/time.h>
#include <assert.h>
#include <stdlib.h>

#include "timeheap.h"

struct th_entry {
    struct timespec t;
    struct th_handle *h;
};

static struct th_entry *heap;
static int heap_cap;
static int heap_use;

static int
cmptime_lt(struct timespec a, struct timespec b)
{
    if (a.tv_sec == b.tv_sec)
        return a.tv_nsec < b.tv_nsec;
    else
        return a.tv_sec < b.tv_sec;
}

static int
cmpentry_lt(int a, int b)
{
    return cmptime_lt(heap[a].t, heap[b].t);
}

static void
swap(int i, int j)
{
    struct th_entry tmp = heap[i];
    heap[i] = heap[j];
    heap[i].h->i = i;
    heap[j] = tmp;
    heap[j].h->i = j;
}

static void
bubble_up(int i)
{
    while (i != 0) {
        int p = (i-1)/2;
        if (cmpentry_lt(i, p)) {
            swap(i, p);
            i = p;
        } else
            return;
    }
}

static void
bubble_down(int i)
{
    int li, ri, ci;
loop:
    li = 2*i+1;
    ri = 2*i+2;
    if (ri < heap_use)
        ci = cmpentry_lt(li, ri) ? li : ri;
    else if (li < heap_use)
        ci = li;
    else
        return;
    if (cmpentry_lt(ci, i)) {
        swap(i, ci);
        i = ci;
        goto loop;
    }
}

int
timeheap_init(void)
{
    heap_cap = 10;
    heap_use = 0;
    if ((heap = malloc(sizeof(struct th_entry) * heap_cap)) == NULL)
        return -1;
    else
        return 0;
}

int
timeheap_size(void)
{
    return heap_use;
}

int
timeheap_insert(struct th_handle *h, struct timespec *t)
{
    if (heap_use == heap_cap) {
        int ncap = heap_cap * 2;
        struct th_entry *nheap = realloc(heap, ncap * sizeof(struct th_entry));
        if (nheap == NULL)
            return -1;
        heap_cap = ncap;
        heap = nheap;
    }
    heap[heap_use].t = *t;
    heap[heap_use].h = h;
    h->i = heap_use;
    heap_use++;
    bubble_up(h->i);
    return 0;
}

void
timeheap_remove(struct th_handle *h)
{
    assert(h->i >= 0 && h->i < heap_use);
    heap_use--;
    if (heap_use > 0) {
        int i = h->i;
        int earlier = cmpentry_lt(heap_use, i);
        heap[i] = heap[heap_use];
        heap[i].h->i = i;
        if (earlier)
            bubble_up(i);
        else
            bubble_down(i);
    }
}

void
timeheap_change(struct th_handle *h, struct timespec *t)
{
    assert(h->i >= 0 && h->i < heap_use);
    int earlier = cmptime_lt(*t, heap[h->i].t);
    heap[h->i].t = *t;
    if (earlier)
        bubble_up(h->i);
    else
        bubble_down(h->i);
}

struct timespec
timeheap_top(void)
{
    return heap[0].t;
}

void *
timeheap_remove_top(void)
{
    void *ret = heap[0].h->data;
    struct th_handle h = { 0, NULL };
    timeheap_remove(&h);
    return ret;
}
