#include <stdarg.h>
#include <stdio.h>

#include "btpd.h"

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

void
btpd_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (BTPD_L_ERROR & btpd_logmask) {
	char tbuf[20];
	time_t tp = time(NULL);
	strftime(tbuf, 20, "%b %e %T", localtime(&tp));
	printf("%s %s: ", tbuf, logtype_str(BTPD_L_ERROR));
	vprintf(fmt, ap);
    }
    va_end(ap);
    exit(1);
}

void
btpd_log(uint32_t type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (type & btpd_logmask) {
	char tbuf[20];
	time_t tp = time(NULL);
	strftime(tbuf, 20, "%b %e %T", localtime(&tp));
	printf("%s %s: ", tbuf, logtype_str(type));
	vprintf(fmt, ap);
    }
    va_end(ap);
}
