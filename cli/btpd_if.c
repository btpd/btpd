#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "benc.h"
#include "iobuf.h"
#include "btpd_if.h"

int
ipc_open(const char *key, struct ipc **out)
{
    size_t plen;
    size_t keylen;
    struct ipc *res;

    if (key == NULL)
        key = "default";
    keylen = strlen(key);
    for (int i = 0; i < keylen; i++)
        if (!isalnum(key[i]))
            return EINVAL;

    res = malloc(sizeof(*res));
    if (res == NULL)
        return ENOMEM;

    plen = sizeof(res->addr.sun_path);
    if (snprintf(res->addr.sun_path, plen,
                 "/tmp/btpd_%u_%s", geteuid(), key) >= plen) {
        free(res);
        return ENAMETOOLONG;
    }
    res->addr.sun_family = AF_UNIX;
    *out = res;
    return 0;
}

int
ipc_close(struct ipc *ipc)
{
    free(ipc);
    return 0;
}

static int
ipc_connect(struct ipc *ipc, FILE **out)
{
    FILE *fp;
    int sd;
    int error;

    if ((sd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1)
        return errno;

    if (connect(sd, (struct sockaddr *)&ipc->addr, sizeof(ipc->addr)) == -1)
        goto error;

    if ((fp = fdopen(sd, "r+")) == NULL)
        goto error;

    *out = fp;
    return 0;
error:
    error = errno;
    close(sd);
    return error;
}

static int
ipc_response(FILE *fp, char **out, uint32_t *len)
{
    uint32_t size;
    char *buf;

    if (fread(&size, sizeof(size), 1, fp) != 1) {
        if (ferror(fp))
            return errno;
        else
            return ECONNRESET;
    }

    if (size == 0)
        return EINVAL;

    if ((buf = malloc(size)) == NULL)
        return ENOMEM;

    if (fread(buf, 1, size, fp) != size) {
        if (ferror(fp))
            return errno;
        else
            return ECONNRESET;
    }

    *out = buf;
    *len = size;
    return 0;
}

static int
ipc_req_res(struct ipc *ipc,
            const char *req, uint32_t qlen,
            char **res, uint32_t *rlen)
{
    FILE *fp;
    int error;

    if ((error = ipc_connect(ipc, &fp)) != 0)
        return error;

    if (fwrite(&qlen, sizeof(qlen), 1, fp) != 1)
        goto error;
    if (fwrite(req, 1, qlen, fp) != qlen)
        goto error;
    if (fflush(fp) != 0)
        goto error;
    if ((errno = ipc_response(fp, res, rlen)) != 0)
        goto error;
    if ((errno = benc_validate(*res, *rlen)) != 0)
        goto error;

    fclose(fp);
    return 0;
error:
    error = errno;
    fclose(fp);
    return error;
}

int
btpd_die(struct ipc *ipc)
{
    int error;
    char *response = NULL;
    const char shutdown[] = "l3:diee";
    uint32_t size = sizeof(shutdown) - 1;
    uint32_t rsiz;

    if ((error = ipc_req_res(ipc, shutdown, size, &response, &rsiz)) != 0)
        return error;

    error = benc_validate(response, rsiz);

    if (error == 0) {
        int64_t tmp;
        benc_dget_int64(response, "code", &tmp);
        error = tmp;
    }

    free(response);
    return error;
}

int
btpd_add(struct ipc *ipc, char **paths, unsigned npaths, char **out)
{
    int error;
    struct io_buffer iob;
    char *res = NULL;
    uint32_t reslen;

    buf_init(&iob, 1024);
    buf_print(&iob, "l3:add");
    for (unsigned i = 0; i < npaths; i++) {
        int plen = strlen(paths[i]);
        buf_print(&iob, "%d:", plen);
        buf_write(&iob, paths[i], plen);
    }
    buf_print(&iob, "e");

    error = ipc_req_res(ipc, iob.buf, iob.buf_off, &res, &reslen);
    free(iob.buf);
    if (error == 0)
        *out = res;

    return error;
}

int
btpd_stat(struct ipc *ipc, char **out)
{
    const char cmd[] = "l4:state";
    uint32_t cmdlen = sizeof(cmd) - 1;
    char *res;
    uint32_t reslen;

    if ((errno = ipc_req_res(ipc, cmd, cmdlen, &res, &reslen)) != 0)
        return errno;
    *out = res;
    return 0;
}

int
btpd_del(struct ipc *ipc, uint8_t (*hash)[20], unsigned nhashes, char **out)
{
    int error;
    struct io_buffer iob;
    char *res = NULL;
    uint32_t reslen;

    buf_init(&iob, 1024);
    buf_write(&iob, "l3:del", 6);
    for (unsigned i = 0; i < nhashes; i++) {
        buf_write(&iob, "20:", 3);
        buf_write(&iob, hash[i], 20);
    }
    buf_write(&iob, "e", 1);

    error = ipc_req_res(ipc, iob.buf, iob.buf_off, &res, &reslen);
    free(iob.buf);
    if (error != 0)
        return error;

    *out = res;
    return 0;
}
