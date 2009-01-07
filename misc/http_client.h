#ifndef BTPD_HTTP_CLIENT_H
#define BTPD_HTTP_CLIENT_H

struct http_url {
    char *host;
    char *uri;
    uint16_t port;
};

struct http_url *http_url_parse(const char *url);
void http_url_free(struct http_url *url);

struct http_response {
    enum {
        HTTP_T_ERR, HTTP_T_CODE, HTTP_T_HEADER, HTTP_T_DATA, HTTP_T_DONE
    } type;
    union {
        int error;
        int code;
        struct {
            char *n;
            char *v;
        } header;
        struct {
            size_t l;
            char *p;
        } data;
    } v;
};

struct http_req;
typedef void (*http_cb_t)(struct http_req *, struct http_response *, void *);

int http_get(struct http_req **out, const char *url, const char *hdrs,
    http_cb_t cb, void *arg);
void http_cancel(struct http_req *req);
struct http_url *http_url_get(struct http_req *req);
int http_want_read(struct http_req *req);
int http_want_write(struct http_req *req);
int http_read(struct http_req *req, int sd);
int http_write(struct http_req *req, int sd);

#endif
