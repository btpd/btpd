#include "btpd.h"

#include <stdarg.h>
#include <time.h>

void *
btpd_malloc(size_t size)
{
    void *a;
    if ((a = malloc(size)) == NULL)
        btpd_err("Failed to allocate %d bytes.\n", (int)size);
    return a;
}

void *
btpd_calloc(size_t nmemb, size_t size)
{
    void *a;
    if ((a = calloc(nmemb, size)) == NULL)
        btpd_err("Failed to allocate %d bytes.\n", (int)(nmemb * size));
    return a;
}

void
btpd_ev_new(struct fdev *ev, int fd, uint16_t flags, evloop_cb_t cb, void *arg)
{
    if (fdev_new(ev, fd, flags, cb, arg) != 0)
        btpd_err("Failed to add event (%s).\n", strerror(errno));
}

void
btpd_ev_del(struct fdev *ev)
{
    if (fdev_del(ev) != 0)
        btpd_err("Failed to remove event (%s).\n", strerror(errno));
}

void
btpd_ev_enable(struct fdev *ev, uint16_t flags)
{
    if (fdev_enable(ev, flags) != 0)
        btpd_err("Failed to enable event (%s).\n", strerror(errno));
}

void
btpd_ev_disable(struct fdev *ev, uint16_t flags)
{
    if (fdev_disable(ev, flags) != 0)
        btpd_err("Failed to disable event (%s).\n", strerror(errno));
}

void
btpd_timer_add(struct timeout *to, struct timespec *ts)
{
    if (timer_add(to, ts) != 0)
        btpd_err("Failed to add timeout (%s).\n", strerror(errno));
}

void
btpd_timer_del(struct timeout *to)
{
    timer_del(to);
}

static const char *
logtype_str(uint32_t type)
{
    if (type & BTPD_L_BTPD)
        return "btpd";
    else if (type & BTPD_L_ERROR)
        return "error";
    else if (type & BTPD_L_CONN)
        return "conn";
    else if (type & BTPD_L_TRACKER)
        return "tracker";
    else if (type & BTPD_L_MSG)
        return "msg";
    else
        return "";
}

static void
log_common(uint32_t type, const char *fmt, va_list ap)
{
    if (type & btpd_logmask) {
        char tbuf[20];
        time_t tp = time(NULL);
        strftime(tbuf, 20, "%b %e %T", localtime(&tp));
        printf("%s %s: ", tbuf, logtype_str(type));
        vprintf(fmt, ap);
    }
}

void
btpd_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_common(BTPD_L_ERROR, fmt, ap);
    va_end(ap);
    exit(1);
}

void
btpd_log(uint32_t type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_common(type, fmt, ap);
    va_end(ap);
}
