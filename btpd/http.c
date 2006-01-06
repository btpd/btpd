#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#include "btpd.h"
#include "http.h"

#define MAX_DOWNLOAD (256 << 10)

enum http_state {
    HS_ADD,
    HS_ACTIVE,
    HS_DONE,
    HS_NOADD,
    HS_CANCEL
};

struct http {
    enum http_state state;
    char *url;
    CURL *curlh;
    struct http_res res;
    BTPDQ_ENTRY(http) entry;
    void (*cb)(struct http *, struct http_res *, void *);
    void *cb_arg;
};

BTPDQ_HEAD(http_tq, http);

static pthread_t m_httptd;
static  struct http_tq m_httpq = BTPDQ_HEAD_INITIALIZER(m_httpq);
static pthread_mutex_t m_httpq_lock;
static pthread_cond_t m_httpq_cond;
static CURLM *m_curlh;

static size_t
http_write_cb(void *ptr, size_t size, size_t nmemb, void *arg)
{
    char *mem;
    struct http_res *res = arg;
    size_t nbytes = size * nmemb;
    size_t nlength = res->length + nbytes;
    if (nlength > MAX_DOWNLOAD)
        return 0;
    if ((mem = realloc(res->content, nlength)) == NULL)
        return 0;
    res->content = mem;
    bcopy(ptr, res->content + res->length, nbytes);
    res->length = nlength;
    return nbytes;
}

int
http_get(struct http **ret,
    void (*cb)(struct http *, struct http_res *, void *),
    void *arg,
    const char *fmt, ...)
{
    struct http *h = btpd_calloc(1, sizeof(*h));

    h->state = HS_ADD;
    h->cb = cb;
    h->cb_arg = arg;
    if ((h->curlh = curl_easy_init()) == NULL)
        btpd_err("Fatal error in curl.\n");

    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&h->url, fmt, ap) == -1)
        btpd_err("Out of memory.\n");
    va_end(ap);

    curl_easy_setopt(h->curlh, CURLOPT_URL, h->url);
    curl_easy_setopt(h->curlh, CURLOPT_USERAGENT, BTPD_VERSION);
    curl_easy_setopt(h->curlh, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(h->curlh, CURLOPT_WRITEDATA, &h->res);
    curl_easy_setopt(h->curlh, CURLOPT_FOLLOWLOCATION, 1);

    pthread_mutex_lock(&m_httpq_lock);
    BTPDQ_INSERT_TAIL(&m_httpq, h, entry);
    pthread_mutex_unlock(&m_httpq_lock);
    pthread_cond_signal(&m_httpq_cond);

    if (ret != NULL)
        *ret = h;

    return 0;
}

void
http_cancel(struct http *http)
{
    pthread_mutex_lock(&m_httpq_lock);
    if (http->state == HS_ADD)
        http->state = HS_NOADD;
    else if (http->state == HS_ACTIVE)
        http->state = HS_CANCEL;
    pthread_mutex_unlock(&m_httpq_lock);
}

int
http_succeeded(struct http_res *res)
{
    return res->res == HRES_OK && res->code >= 200 && res->code < 300;
}

static void
http_td_cb(void *arg)
{
    struct http *h = arg;
    if (h->res.res == HRES_OK)
        curl_easy_getinfo(h->curlh, CURLINFO_RESPONSE_CODE, &h->res.code);
    if (h->res.res == HRES_FAIL) {
        btpd_log(BTPD_L_BTPD, "Http error for url '%s' (%s).\n", h->url,
            curl_easy_strerror(h->res.code));
    }
    h->cb(h, &h->res, h->cb_arg);
    curl_easy_cleanup(h->curlh);
    if (h->res.content != NULL)
        free(h->res.content);
    free(h->url);
    free(h);
}

static void
http_td_actions(void)
{
    int has_posted = 0;
    struct http *http, *next;
    int nmsgs;
    CURLMsg *cmsg;

    BTPDQ_FOREACH_MUTABLE(http, &m_httpq, entry, next) {
        switch (http->state) {
        case HS_ADD:
            curl_multi_add_handle(m_curlh, http->curlh);
            http->state = HS_ACTIVE;
            break;
        case HS_CANCEL:
            curl_multi_remove_handle(m_curlh, http->curlh);
        case HS_NOADD:
            BTPDQ_REMOVE(&m_httpq, http, entry);
            http->state = HS_CANCEL;
            http->res.res = HRES_CANCEL;
            if (!has_posted) {
                has_posted = 1;
                td_post_begin();
            }
            td_post(http_td_cb, http);
            break;
        case HS_DONE:
            abort();
        default:
            break;
        }
    }

    while ((cmsg = curl_multi_info_read(m_curlh, &nmsgs)) != NULL) {
        BTPDQ_FOREACH(http, &m_httpq, entry) {
            if (http->curlh == cmsg->easy_handle)
                break;
        }
        assert(http != NULL);
        BTPDQ_REMOVE(&m_httpq, http, entry);
        http->state = HS_DONE;
        if (cmsg->data.result == 0)
            http->res.res = HRES_OK;
        else {
            http->res.res = HRES_FAIL;
            http->res.code = cmsg->data.result;
        }
        curl_multi_remove_handle(m_curlh, http->curlh);
        if (!has_posted) {
            td_post_begin();
            has_posted = 1;
        }
        td_post(http_td_cb, http);
    }

    if (has_posted)
        td_post_end();
}

static void
http_td_curl(void)
{
    fd_set rset, wset, eset;
    int maxfd, nbusy;

    pthread_mutex_unlock(&m_httpq_lock);

    do {
        while (CURLM_CALL_MULTI_PERFORM == curl_multi_perform(m_curlh, &nbusy))
            ;

        FD_ZERO(&rset);
        FD_ZERO(&wset);
        FD_ZERO(&eset);

        curl_multi_fdset(m_curlh, &rset, &wset, &eset, &maxfd);
        select(maxfd + 1, &rset, &wset, &eset, (& (struct timeval) { 2, 0}));

        pthread_mutex_lock(&m_httpq_lock);
        http_td_actions();
        pthread_mutex_unlock(&m_httpq_lock);
    } while (nbusy > 0);

    pthread_mutex_lock(&m_httpq_lock);
}

static void *
http_td(void *arg)
{
    pthread_mutex_lock(&m_httpq_lock);
    for (;;) {
        while (BTPDQ_EMPTY(&m_httpq))
            pthread_cond_wait(&m_httpq_cond, &m_httpq_lock);
        http_td_actions();
        if (!BTPDQ_EMPTY(&m_httpq))
            http_td_curl();
    }
}

void
http_init(void)
{
    int err;
    if (curl_global_init(0))
        goto curl_err;
    if ((m_curlh = curl_multi_init()) == NULL)
        goto curl_err;

    err = pthread_mutex_init(&m_httpq_lock, NULL);
    if (err == 0)
        err = pthread_cond_init(&m_httpq_cond, NULL);
    if (err == 0)
        err = pthread_create(&m_httptd, NULL, http_td, NULL);
    if (err != 0)
        btpd_err("pthread failure (%s)\n", strerror(err));
    return;
curl_err:
    btpd_err("Fatal error in curl.\n");
}
