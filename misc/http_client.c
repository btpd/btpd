#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "iobuf.h"
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
        PS_HEAD, PS_CHUNK_SIZE, PS_CHUNK_DATA, PS_CHUNK_CRLF, PS_ID_DATA
    } pstate;

    int parsing;
    int cancel;
    int chunked;
    long length;

    http_cb_t cb;
    void *arg;

    struct http_url *url;
    struct iobuf rbuf;
    struct iobuf wbuf;
};

static void
http_free(struct http_req *req)
{
    if (req->url != NULL)
        http_url_free(req->url);
    iobuf_free(&req->rbuf);
    iobuf_free(&req->wbuf);
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

static char *
strnl(char *str, int *nlsize)
{
    char *nl = strchr(str, '\n');
    if (nl != NULL && nl > str && *(nl - 1) == '\r') {
        *nlsize = 2;
        return nl - 1;
    } else {
        *nlsize = 1;
        return nl;
    }
}

static int
headers_parse(struct http_req *req, char *buf, char *end)
{
    int code, majv, minv, nlsize;
    char *cur, *nl;
    char name[128], value[872];
    struct http_response res;

    req->chunked = 0;
    req->length = -1;

    if (sscanf(buf, "HTTP/%d.%d %d", &majv, &minv, &code) != 3)
        return 0;
    res.type = HTTP_T_CODE;
    res.v.code = code;
    req->cb(req, &res, req->arg);
    if (req->cancel)
        return 1;

    cur = strchr(buf, '\n') + 1;
    nl = strnl(cur, &nlsize);
    while (cur < end) {
        int i;
        char *colon = strchr(cur, ':');
        if (colon == NULL || colon > nl)
            return 0;
        snprintf(name, sizeof(name), "%.*s", (int)(colon - cur), cur);

        cur = colon + 1;
        i = 0;
    val_loop:
        while (isblank(*cur))
            cur++;
        while (cur < nl) {
            if (i < sizeof(value) - 1) {
                value[i] = *cur;
                i++;
            }
            cur++;
        }
        cur += nlsize;
        nl = strnl(cur, &nlsize);
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
        if ((end = iobuf_find(&req->rbuf, "\r\n\r\n", 4)) != NULL)
            dlen = 4;
        else if ((end = iobuf_find(&req->rbuf, "\n\n", 2)) != NULL)
            dlen = 2;
        else {
            if (req->rbuf.off < (1 << 15))
                return 1;
            else
                goto error;
        }
        if (!iobuf_write(&req->rbuf, "", 1))
            goto error;
        req->rbuf.off--;
        if (!headers_parse(req, req->rbuf.buf, end))
            goto error;
        if (req->cancel)
            goto cancel;
        iobuf_consumed(&req->rbuf, end - (char *)req->rbuf.buf + dlen);
        goto again;
    case PS_CHUNK_SIZE:
        assert(req->chunked);
        if (len == 0)
            goto error;
        if ((end = iobuf_find(&req->rbuf, "\n", 1)) == NULL) {
            if (req->rbuf.off < 20)
                return 1;
            else
                goto error;
        }
        errno = 0;
        req->length = strtol(req->rbuf.buf, &numend, 16);
        if (req->length < 0 || numend == (char *)req->rbuf.buf || errno)
            goto error;
        if (req->length == 0)
            goto done;
        iobuf_consumed(&req->rbuf, end - (char *)req->rbuf.buf + 1);
        req->pstate = PS_CHUNK_DATA;
        goto again;
    case PS_CHUNK_DATA:
        if (len == 0)
            goto error;
        assert(req->length > 0);
        dlen = min(req->rbuf.off, req->length);
        if (dlen > 0) {
            res.type = HTTP_T_DATA;
            res.v.data.l = dlen;
            res.v.data.p = req->rbuf.buf;
            req->cb(req, &res, req->arg);
            if (req->cancel)
                goto cancel;
            iobuf_consumed(&req->rbuf, dlen);
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
        if (req->rbuf.off < 2)
            return 1;
        if (req->rbuf.buf[0] == '\r' && req->rbuf.buf[1] == '\n')
            dlen = 2;
        else if (req->rbuf.buf[0] == '\n')
            dlen = 1;
        else
            goto error;
        iobuf_consumed(&req->rbuf, dlen);
        req->pstate = PS_CHUNK_SIZE;
        goto again;
    case PS_ID_DATA:
        if (len == 0 && req->length < 0)
            goto done;
        else if (len == 0)
            goto error;
        if (req->length < 0)
            dlen = req->rbuf.off;
        else
            dlen = min(req->rbuf.off, req->length);
        if (dlen > 0) {
            res.type = HTTP_T_DATA;
            res.v.data.p = req->rbuf.buf;
            res.v.data.l = dlen;
            req->cb(req, &res, req->arg);
            if (req->cancel)
                goto cancel;
            iobuf_consumed(&req->rbuf, dlen);
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

struct http_url *
http_url_get(struct http_req *req)
{
    return req->url;
}

int
http_want_read(struct http_req *req)
{
    return 1;
}

int
http_want_write(struct http_req *req)
{
    return req->wbuf.off > 0;
}

int
http_read(struct http_req *req, int sd)
{
    if (!iobuf_accommodate(&req->rbuf, 4096)) {
        http_error(req);
        return 0;
    }
    ssize_t nr = read(sd, req->rbuf.buf + req->rbuf.off, 4096);
    if (nr < 0 && errno == EAGAIN)
        return 1;
    else if (nr < 0) {
        http_error(req);
        return 0;
    } else {
        req->rbuf.off += nr;
        req->parsing = 1;
        if (http_parse(req, nr)) {
            req->parsing = 0;
            return 1;
        } else
            return 0;
    }
}

int
http_write(struct http_req *req, int sd)
{
    assert(req->wbuf.off > 0);
    ssize_t nw =
        write(sd, req->wbuf.buf, req->wbuf.off);
    if (nw < 0 && errno == EAGAIN)
        return 1;
    else if (nw < 0) {
        http_error(req);
        return 0;
    } else {
        iobuf_consumed(&req->wbuf, nw);
        return 1;
    }
}

int
http_get(struct http_req **out, const char *url, const char *hdrs,
    http_cb_t cb, void *arg)
{
    struct http_req *req = calloc(1, sizeof(*req));
    if (req == NULL)
        return 0;
    req->cb = cb;
    req->arg = arg;
    req->url = http_url_parse(url);
    if (req->url == NULL)
        goto error;
    req->rbuf = iobuf_init(4096);
    req->wbuf = iobuf_init(1024);
    if (!iobuf_print(&req->wbuf, "GET %s HTTP/1.1\r\n"
            "Host: %s:%hu\r\n"
            "Accept-Encoding:\r\n"
            "Connection: close\r\n"
            "%s"
            "\r\n", req->url->uri, req->url->host, req->url->port, hdrs))
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
    if (req->parsing)
        req->cancel = 1;
    else
        http_free(req);
}
