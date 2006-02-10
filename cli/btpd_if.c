#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "benc.h"
#include "btpd_if.h"
#include "iobuf.h"
#include "subr.h"

struct ipc {
    int sd;
};

int
ipc_open(const char *dir, struct ipc **out)
{
    int sd = -1, err = 0;
    size_t plen;
    struct ipc *res;
    struct sockaddr_un addr;

    plen = sizeof(addr.sun_path);
    if (snprintf(addr.sun_path, plen, "%s/sock", dir) >= plen)
        return ENAMETOOLONG;
    addr.sun_family = AF_UNIX;

    if ((sd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1)
        return errno;

    if (connect(sd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        err = errno;
        close(sd);
        return err;
    }

    if ((res = malloc(sizeof(*res))) == NULL) {
        close(sd);
        return ENOMEM;
    }

    res->sd = sd;
    *out = res;
    return 0;
}

int
ipc_close(struct ipc *ipc)
{
    int err;
    err = close(ipc->sd);
    free(ipc);
    return err;
}

static int
ipc_response(struct ipc *ipc, char **out, uint32_t *len)
{
    uint32_t size;
    char *buf;

    if ((errno = read_fully(ipc->sd, &size, sizeof(size))) != 0)
        return errno;

    if (size == 0)
        return ECONNRESET;

    if ((buf = malloc(size)) == NULL)
        return ENOMEM;

    if ((errno = read_fully(ipc->sd, buf, size)) != 0) {
        free(buf);
        return errno;
    }

    *out = buf;
    *len = size;
    return 0;
}

static int
ipc_req_res(struct ipc *ipc, const char *req, uint32_t qlen, char **res,
    uint32_t *rlen)
{
    if ((errno = write_fully(ipc->sd, &qlen, sizeof(qlen))) != 0)
        goto error;
    if ((errno = write_fully(ipc->sd, req, qlen)) != 0)
        goto error;
    if ((errno = ipc_response(ipc, res, rlen)) != 0)
        goto error;
    if ((errno = benc_validate(*res, *rlen)) != 0)
        goto error;
    if (!benc_isdct(*res))
        errno = EINVAL;
error:
    return errno;
}

static enum ipc_code
ipc_buf_req(struct ipc *ipc, struct io_buffer *iob)
{
    int err;
    char *res;
    size_t reslen;

    err = ipc_req_res(ipc, iob->buf, iob->buf_off, &res, &reslen);
    free(iob->buf);
    if (err != 0)
        return IPC_COMMERR;
    int code;
    code = benc_dget_int(res, "code");
    free(res);
    return code;
}

enum ipc_code
btpd_die(struct ipc *ipc, int seconds)
{
    struct io_buffer iob;
    buf_init(&iob, 16);
    if (seconds >= 0)
        buf_print(&iob, "l3:diei%dee", seconds);
    else
        buf_swrite(&iob, "l3:diee");
    return ipc_buf_req(ipc, &iob);
}

enum ipc_code
parse_btstat(const uint8_t *res, struct btstat **out)
{
    int code;
    unsigned ntorrents;
    const char *tlst;

    code = benc_dget_int(res, "code");
    if (code != IPC_OK)
        return code;

    ntorrents = benc_dget_int(res, "ntorrents");
    tlst = benc_dget_lst(res, "torrents");

    struct btstat *st =
        malloc(sizeof(struct btstat) + sizeof(struct tpstat) * ntorrents);

    st->ntorrents = ntorrents;
    int i = 0;
    for (const char *tp = benc_first(tlst); tp != NULL; tp = benc_next(tp)) {
        struct tpstat *ts = &st->torrents[i];
        ts->hash = benc_dget_mema(tp, "info hash", NULL);
        ts->name = benc_dget_str(tp, "name", NULL);
        ts->state = benc_dget_int(tp, "state");
        ts->peers = benc_dget_int(tp, "peers");
        ts->tr_errors = benc_dget_int(tp, "tracker errors");
        ts->content_got = benc_dget_int(tp, "content got");
        ts->content_size = benc_dget_int(tp, "content size");
        ts->pieces_got = benc_dget_int(tp, "pieces got");
        ts->pieces_seen = benc_dget_int(tp, "pieces seen");
        ts->torrent_pieces = benc_dget_int(tp, "torrent pieces");
        ts->downloaded = benc_dget_int(tp, "downloaded");
        ts->uploaded = benc_dget_int(tp, "uploaded");
        ts->rate_down = benc_dget_int(tp, "rate down");
        ts->rate_up = benc_dget_int(tp, "rate up");
        i++;
    }
    *out = st;
    return IPC_OK;
}

void
free_btstat(struct btstat *st)
{
    for (unsigned i = 0; i < st->ntorrents; i++) {
        if (st->torrents[i].hash != NULL)
            free(st->torrents[i].hash);
        if (st->torrents[i].name != NULL)
            free(st->torrents[i].name);
    }
    free(st);
}

enum ipc_code
btpd_stat(struct ipc *ipc, struct btstat **out)
{
    int err;
    const char cmd[] = "l4:state";
    uint32_t cmdlen = sizeof(cmd) - 1;
    char *res;
    uint32_t reslen;

    if ((err = ipc_req_res(ipc, cmd, cmdlen, &res, &reslen)) != 0)
        return IPC_COMMERR;

    err = parse_btstat(res, out);
    free(res);
    return err;
}

enum ipc_code
btpd_add(struct ipc *ipc, const uint8_t *hash, const char *torrent,
    const char *content)
{
    struct io_buffer iob;
    buf_init(&iob, (1 << 10));
    buf_print(&iob, "l3:addd7:content%d:%s4:hash20:", (int)strlen(content),
        content);
    buf_write(&iob, hash, 20);
    buf_print(&iob, "7:torrent%d:%see", (int)strlen(torrent), torrent);
    return ipc_buf_req(ipc, &iob);
}

enum ipc_code
btpd_del(struct ipc *ipc, const uint8_t *hash)
{
    struct io_buffer iob;
    buf_init(&iob, 32);
    buf_swrite(&iob, "l3:del20:");
    buf_write(&iob, hash, 20);
    buf_write(&iob, "e", 1);
    return ipc_buf_req(ipc, &iob);
}
