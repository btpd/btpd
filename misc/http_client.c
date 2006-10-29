#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <event.h>
#include <evdns.h>

#include "subr.h"
#include "http_client.h"

struct http_url *
http_url_parse(const char *url)
{
    size_t ulen = strlen(url);
    if (strncmp(url, "http://", 7) != 0)
        return NULL;
    const char *cur, *at, *uri = NULL, *uri_e = NULL;
    const char *host = NULL, *host_e = NULL;
    const char *port = NULL, *port_e = NULL;
    at = strchr(url + 7, '@');
    uri = strchr(url + 7, '/');
    cur = strchr(url + 7, '?');
    if (cur != NULL && (uri == NULL || cur < uri))
        uri = cur;
    if (uri == NULL)
        uri = url + ulen;
    if (at != NULL && at < uri)
        host = at + 1;
    else
        host = url + 7;
    cur = host;
    while (cur < uri && *cur != ':')
        cur++;
    host_e = cur;
    if (host_e == host)
        return NULL;
    if (*cur == ':') {
        cur++;
        port = cur;
        while (cur < uri && *cur >= '0' && *cur <= '9')
            cur++;
        if (cur == port || cur != uri)
            return NULL;
        port_e = cur;
    }
    while (*cur != '\0')
        cur++;
    uri_e = cur;
    struct http_url *res =
        malloc(sizeof(*res) + host_e - host + 1 + uri_e - uri + 2);
    if (res == NULL)
        return NULL;
    if (port != NULL)
        sscanf(port, "%hu", &res->port);
    else
        res->port = 80;
    res->host = (char *)(res + 1);
    res->uri = res->host + (host_e - host + 1);
    bcopy(host, res->host, host_e - host);
    res->host[host_e - host] = '\0';
    if (*uri != '/') {
        res->uri[0] = '/';
        bcopy(uri, res->uri + 1, uri_e - uri);
        res->uri[uri_e - uri + 1] = '\0';
    } else {
        bcopy(uri, res->uri, uri_e - uri);
        res->uri[uri_e - uri] = '\0';
    }
    return res;
}

void
http_url_free(struct http_url *url)
{
    free(url);
}

struct http_req {
    enum {
        HTTP_RESOLVE, HTTP_CONNECT, HTTP_WRITE, HTTP_RECEIVE, HTTP_PARSE
    } state;
    struct http_url *url;
    int sd;
    struct event ev;
    http_cb cb;
    void *arg;
    int cancel;

    int pstate, chunked;
    long length;

    struct evbuffer *buf;
};

static void
http_free(struct http_req *req)
{
    if (req->url != NULL)
        http_url_free(req->url);
    if (req->buf != NULL)
        evbuffer_free(req->buf);
    if (req->sd > 0) {
        event_del(&req->ev);
        close(req->sd);
    }
    free(req);
}

static void
http_error(struct http_req *req)
{
    struct http_response res;
    res.type = HTTP_T_ERR;
    res.v.error = 1;
    req->cb(req, &res, req->arg);
    http_free(req);
}

#define PS_HEAD  0
#define PS_CHUNK_SIZE 1
#define PS_CHUNK_DATA 2
#define PS_CHUNK_CRLF 3
#define PS_ID_DATA  4

static int
headers_parse(struct http_req *req, char *buf, char *end)
{
    int code;
    char *cur, *crlf;
    char name[128], value[872];
    struct http_response res;

    req->chunked = 0;
    req->length = -1;

    if (sscanf(buf, "HTTP/1.1 %d", &code) == 0)
        return 0;
    res.type = HTTP_T_CODE;
    res.v.code = code;
    req->cb(req, &res, req->arg);
    if (req->cancel)
        return 1;

    cur = strstr(buf, "\r\n") + 2;
    crlf = strstr(cur, "\r\n");
    while (cur < end) {
        int i;
        char *colon = strchr(cur, ':');
        if (colon == NULL || colon > crlf)
            return 0;
        snprintf(name, sizeof(name), "%.*s", (int)(colon - cur), cur);

        cur = colon + 1;
        i = 0;
    val_loop:
        while (isblank(*cur))
            cur++;
        while (cur < crlf) {
            if (i < sizeof(value) - 1) {
                value[i] = *cur;
                i++;
            }
            cur++;
        }
        cur += 2;
        crlf = strstr(cur, "\r\n");
        if (isblank(*cur)) {
            if (i < sizeof(value) - 1) {
                value[i] = ' ';
                i++;
            }
            cur++;
            goto val_loop;
        }
        value[i] = '\0';
        for (i--; i >= 0 && isblank(value[i]); i--)
            value[i] = '\0';

        res.type = HTTP_T_HEADER;
        res.v.header.n = name;
        res.v.header.v = value;
        req->cb(req, &res, req->arg);
        if (req->cancel)
            return 1;
        if ((!req->chunked
                && strcasecmp("Transfer-Encoding", name) == 0
                && strcasecmp("chunked", value) == 0))
            req->chunked = 1;
        if ((!req->chunked && req->length == -1
                && strcasecmp("Content-Length", name) == 0)) {
            errno = 0;
            req->length = strtol(value, NULL, 10);
            if (errno)
                req->length = -1;
        }
    }
    if (req->chunked)
        req->pstate = PS_CHUNK_SIZE;
    else
        req->pstate = PS_ID_DATA;
    return 1;
}

static int
http_parse(struct http_req *req, int len)
{
    char *end, *numend;
    size_t dlen;
    struct http_response res;
again:
    switch (req->pstate) {
    case PS_HEAD:
        if (len == 0)
            goto error;
        if ((end = evbuffer_find(req->buf, "\r\n\r\n", 4)) == NULL) {
            if (req->buf->off < (1 << 15))
                return 1;
            else
                goto error;
        }
        if (evbuffer_add(req->buf, "", 1) != 0)
            goto error;
        req->buf->off--;
        if (!headers_parse(req, req->buf->buffer, end))
            goto error;
        if (req->cancel)
            goto cancel;
        evbuffer_drain(req->buf, end - (char *)req->buf->buffer + 4);
        goto again;
    case PS_CHUNK_SIZE:
        assert(req->chunked);
        if (len == 0)
            goto error;
        if ((end = evbuffer_find(req->buf, "\r\n", 2)) == NULL) {
            if (req->buf->off < 20)
                return 1;
            else
                goto error;
        }
        errno = 0;
        req->length = strtol(req->buf->buffer, &numend, 16);
        if (req->length < 0 || numend == (char *)req->buf->buffer || errno)
            goto error;
        if (req->length == 0)
            goto done;
        evbuffer_drain(req->buf, end - (char *)req->buf->buffer + 2);
        req->pstate = PS_CHUNK_DATA;
        goto again;
    case PS_CHUNK_DATA:
        if (len == 0)
            goto error;
        assert(req->length > 0);
        dlen = min(req->buf->off, req->length);
        if (dlen > 0) {
            res.type = HTTP_T_DATA;
            res.v.data.l = dlen;
            res.v.data.p = req->buf->buffer;
            req->cb(req, &res, req->arg);
            if (req->cancel)
                goto cancel;
            evbuffer_drain(req->buf, dlen);
            req->length -= dlen;
            if (req->length == 0) {
                req->pstate = PS_CHUNK_CRLF;
                goto again;
            }
        }
        return 1;
    case PS_CHUNK_CRLF:
        if (len == 0)
            goto error;
        assert(req->length == 0);
        if (req->buf->off < 2)
            return 1;
        if (bcmp(req->buf->buffer, "\r\n", 2) != 0)
            goto error;
        evbuffer_drain(req->buf, 2);
        req->pstate = PS_CHUNK_SIZE;
        goto again;
    case PS_ID_DATA:
        if (len == 0 && req->length < 0)
            goto done;
        else if (len == 0)
            goto error;
        if (req->length < 0)
            dlen = req->buf->off;
        else
            dlen = min(req->buf->off, req->length);
        if (dlen > 0) {
            res.type = HTTP_T_DATA;
            res.v.data.p = req->buf->buffer;
            res.v.data.l = dlen;
            req->cb(req, &res, req->arg);
            if (req->cancel)
                goto cancel;
            evbuffer_drain(req->buf, dlen);
            if (req->length > 0) {
                req->length -= dlen;
                if (req->length == 0)
                    goto done;
            }
        }
        return 1;
    default:
        abort();
    }
error:
    http_error(req);
    return 0;
done:
    res.type = HTTP_T_DONE;
    req->cb(req, &res, req->arg);
cancel:
    http_free(req);
    return 0;
}

static void
http_read_cb(int sd, short type, void *arg)
{
    struct http_req *req = arg;
    if (type == EV_TIMEOUT) {
        http_error(req);
        return;
    }
    int nr = evbuffer_read(req->buf, sd, 1 << 14);
    if (nr < 0) {
        if (nr == EAGAIN)
            goto more;
        else {
            printf("read err\n");
            http_error(req);
            return;
        }
    }
    req->state = HTTP_PARSE;
    if (!http_parse(req, nr))
        return;
    req->state = HTTP_RECEIVE;
more:
    if (event_add(&req->ev, NULL) != 0)
        http_error(req);
}

static void
http_write_cb(int sd, short type, void *arg)
{
    struct http_req *req = arg;
    if (type == EV_TIMEOUT) {
        http_error(req);
        return;
    }
    int nw = evbuffer_write(req->buf, sd);
    if (nw == -1) {
        if (errno == EAGAIN)
            goto out;
        else
            goto error;
    }
out:
    if (req->buf->off != 0) {
        if (event_add(&req->ev, NULL) != 0)
            goto error;
    } else {
        req->state = HTTP_RECEIVE;
        event_set(&req->ev, req->sd, EV_READ, http_read_cb, req);
        if (event_add(&req->ev, NULL) != 0)
            goto error;
    }
    return;
error:
    printf("http write err\n");
    http_error(req);
}

static void
http_dnscb(int result, char type, int count, int ttl, void *addrs, void *arg)
{
    struct http_req *req = arg;
    if (req->cancel)
        http_free(req);
    else if (result == 0 && type == 1 && count > 0) {
        int addri = rand_between(0, count - 1);
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(req->url->port);
        bcopy(addrs + addri * 4, &addr.sin_addr.s_addr, 4);
        req->state = HTTP_CONNECT;
        if ((req->sd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
            goto error;
        if (set_nonblocking(req->sd) != 0)
            goto error;
        if ((connect(req->sd, (struct sockaddr *)&addr, sizeof(addr)) != 0
                && errno != EINPROGRESS))
            goto error;
        event_set(&req->ev, req->sd, EV_WRITE, http_write_cb, req);
        if (event_add(&req->ev, NULL) != 0)
            goto error;
    } else
        goto error;
    return;

error:
    http_error(req);
}

int
http_get(struct http_req **out, const char *url, const char *hdrs, http_cb cb,
    void *arg)
{
    struct http_req *req = calloc(1, sizeof(*req));
    if (req == NULL)
        return 0;
    req->sd = -1;
    req->cb = cb;
    req->arg = arg;
    req->url = http_url_parse(url);
    if (req->url == NULL)
        goto error;
    if ((req->buf = evbuffer_new()) == NULL)
        goto error;
    if (evbuffer_add_printf(req->buf, "GET %s HTTP/1.1\r\n"
            "Accept-Encoding:\r\n"
            "Connection: close\r\n"
            "Host: %s\r\n"
            "%s"
            "\r\n", req->url->uri, req->url->host, hdrs) == -1)
        goto error;
    if (evdns_resolve_ipv4(req->url->host, 0, http_dnscb, req) != 0)
        goto error;
    if (out != NULL)
        *out = req;
    return 1;
error:
    http_free(req);
    return 0;
}

void
http_cancel(struct http_req *req)
{
    if (req->state == HTTP_RESOLVE || req->state == HTTP_PARSE)
        req->cancel = 1;
    else
        http_free(req);
}
