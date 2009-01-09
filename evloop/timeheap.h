#ifndef BTPD_TIMEHEAP_H
#define BTPD_TIMEHEAP_H

struct th_handle {
    int i;
    void *data;
};

int timeheap_init(void);
int timeheap_size(void);

int  timeheap_insert(struct th_handle *h, struct timespec *t);
void timeheap_remove(struct th_handle *h);
void timeheap_change(struct th_handle *h, struct timespec *t);

void *timeheap_remove_top(void);
struct timespec timeheap_top(void);

#endif
