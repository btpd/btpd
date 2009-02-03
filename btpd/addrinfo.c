#include "btpd.h"

#include <pthread.h>

struct ai_ctx {
    BTPDQ_ENTRY(ai_ctx) entry;
    struct addrinfo hints;
    struct addrinfo *res;
    char node[255], service[6];
    void (*cb)(void *, int, struct addrinfo *);
    void *arg;
    int cancel;
    int error;
    uint16_t port;
};

BTPDQ_HEAD(ai_ctx_tq, ai_ctx);

static struct ai_ctx_tq m_aiq = BTPDQ_HEAD_INITIALIZER(m_aiq);
static pthread_mutex_t m_aiq_lock;
static pthread_cond_t m_aiq_cond;

struct ai_ctx *
btpd_addrinfo(const char *node, uint16_t port, struct addrinfo *hints,
    void (*cb)(void *, int, struct addrinfo *), void *arg)
{
    struct ai_ctx *ctx = btpd_calloc(1, sizeof(*ctx));
    ctx->hints = *hints;
    ctx->cb = cb;
    ctx->arg = arg;    
    snprintf(ctx->node, sizeof(ctx->node), "%s", node);
    ctx->port = port;
    snprintf(ctx->service, sizeof(ctx->service), "%hu", port);

    pthread_mutex_lock(&m_aiq_lock);
    BTPDQ_INSERT_TAIL(&m_aiq, ctx, entry);
    pthread_mutex_unlock(&m_aiq_lock);
    pthread_cond_signal(&m_aiq_cond);

    return ctx;
}

void
btpd_addrinfo_cancel(struct ai_ctx *ctx)
{
    ctx->cancel = 1;
}

static void
addrinfo_td_cb(void *arg)
{
    struct ai_ctx *ctx = arg;
    if (!ctx->cancel)
        ctx->cb(ctx->arg, ctx->error, ctx->res);
    else if (ctx->error != 0)
        freeaddrinfo(ctx->res);
    free(ctx);
}

static void *
addrinfo_td(void *arg)
{
    struct ai_ctx *ctx;
    while (1) {
        pthread_mutex_lock(&m_aiq_lock);
        while (BTPDQ_EMPTY(&m_aiq))
            pthread_cond_wait(&m_aiq_cond, &m_aiq_lock);
        ctx = BTPDQ_FIRST(&m_aiq);
        BTPDQ_REMOVE(&m_aiq, ctx, entry);
        pthread_mutex_unlock(&m_aiq_lock);

        ctx->error =
            getaddrinfo(ctx->node, ctx->service, &ctx->hints, &ctx->res);

        td_post_begin();
        td_post(addrinfo_td_cb, ctx);
        td_post_end();
    }
}

static void
errdie(int err, const char *str)
{
    if (err != 0)
        btpd_err("addrinfo_init: %s (%s).\n", str, strerror(errno));
}

void
addrinfo_init(void)
{
    pthread_t td;
    errdie(pthread_mutex_init(&m_aiq_lock, NULL), "pthread_mutex_init");
    errdie(pthread_cond_init(&m_aiq_cond, NULL), "pthread_cond_init");
    errdie(pthread_create(&td, NULL, addrinfo_td, NULL), "pthread_create");
}
