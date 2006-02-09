#ifndef BTPD_HTTP_H
#define BTPD_HTTP_H

struct http;

enum http_status {
    HRES_OK,
    HRES_FAIL,
    HRES_CANCEL
};

struct http_res {
    enum http_status res;
    long code;
    char *content;
    size_t length;
};

int http_get(struct http **ret,
    void (*cb)(struct http *, struct http_res *, void *),
    void *arg,
    const char *fmt, ...);
void http_cancel(struct http *http);
int http_succeeded(struct http_res *res);

void http_init(void);

#endif
