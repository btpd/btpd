#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <assert.h>
#include <ctype.h>
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

static const char *errmsgs[] = {
#define ERRDEF(name, msg) msg,
#include "ipcdefs.h"
#undef ERRDEF
    NULL
};

static const char *tval_names[] = {
#define TVDEF(val, type, name) name,
#include "ipcdefs.h"
#undef TVDEF
    NULL
};

const char *
ipc_strerror(enum ipc_err err)
{
    if (err < 0 || err >= IPC_ERRCOUNT)
        return "unknown error";
    return errmsgs[err];
}

const char *
tval_name(enum ipc_tval key)
{
    if (key < 0 || key >= IPC_TVALCOUNT)
        return "unknown key";
    return tval_names[key];
}

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

void
ipc_close(struct ipc *ipc)
{
    close(ipc->sd);
    free(ipc);
}

static enum ipc_err
ipc_response(struct ipc *ipc, char **out, uint32_t *len)
{
    uint32_t size;
    char *buf;

    if (read_fully(ipc->sd, &size, sizeof(size)) != 0)
        return IPC_COMMERR;
    if (size == 0)
        return IPC_COMMERR;
    if ((buf = malloc(size)) == NULL)
        return IPC_COMMERR;
    if (read_fully(ipc->sd, buf, size) != 0) {
        free(buf);
        return IPC_COMMERR;
    }
    *out = buf;
    *len = size;
    return IPC_OK;
}

static enum ipc_err
ipc_req_res(struct ipc *ipc, const char *req, uint32_t qlen, char **res,
    uint32_t *rlen)
{
    if (write_fully(ipc->sd, &qlen, sizeof(qlen)) != 0)
        return IPC_COMMERR;
    if (write_fully(ipc->sd, req, qlen) != 0)
        return IPC_COMMERR;
    if (ipc_response(ipc, res, rlen) != 0)
        return IPC_COMMERR;
    if (benc_validate(*res, *rlen) != 0)
        return IPC_COMMERR;
    if (!benc_isdct(*res))
        return IPC_COMMERR;
    return IPC_OK;
}

static enum ipc_err
ipc_buf_req_res(struct ipc *ipc, struct iobuf *iob, char **res,
    uint32_t *rlen)
{
    enum ipc_err err;
    if (iob->error)
        err = IPC_COMMERR;
    else
        err = ipc_req_res(ipc, iob->buf, iob->off, res, rlen);
    iobuf_free(iob);
    return err;
}

static enum ipc_err
ipc_buf_req_code(struct ipc *ipc, struct iobuf *iob)
{
    enum ipc_err err;
    char *res;
    uint32_t rlen;

    if ((err = ipc_buf_req_res(ipc, iob, &res, &rlen)) == 0) {
        err = benc_dget_int(res, "code");
        free(res);
    }
    return err;
}

enum ipc_err
btpd_die(struct ipc *ipc, int seconds)
{
    struct iobuf iob = iobuf_init(16);
    if (seconds >= 0)
        iobuf_print(&iob, "l3:diei%dee", seconds);
    else
        iobuf_swrite(&iob, "l3:diee");
    return ipc_buf_req_code(ipc, &iob);
}

static enum ipc_err
tget_common(char *ans, enum ipc_tval *keys, size_t nkeys, tget_cb_t cb,
    void *arg)
{
    int err;
    const char *res;
    struct ipc_get_res cbres[IPC_TVALCOUNT];

    if ((err = benc_dget_int(ans, "code")) != 0)
        return err;

    res = benc_dget_lst(ans, "result");
    int obji = 0;
    for (res = benc_first(res); res != NULL; res = benc_next(res)) {
        if (benc_isint(res)) {
            cb(obji, benc_int(res, NULL), NULL, arg);
            obji++;
            continue;
        }
        const char *t = benc_first(res);
        const char *v = benc_next(t);
        for (int j = 0; j < nkeys; j++) {
            cbres[keys[j]].type = benc_int(t, NULL);
            switch (cbres[keys[j]].type) {
            case IPC_TYPE_ERR:
            case IPC_TYPE_NUM:
                cbres[keys[j]].v.num = benc_int(v, NULL);
                break;
            case IPC_TYPE_STR:
            case IPC_TYPE_BIN:
                cbres[keys[j]].v.str.p= benc_mem(v, &cbres[keys[j]].v.str.l,
                    NULL);
                break;
            }
            t = benc_next(v);
            if (t != NULL)
                v = benc_next(t);
        }
        cb(obji, IPC_OK, cbres, arg);
        obji++;
    }

    free(ans);
    return IPC_OK;
}

enum ipc_err
btpd_tget(struct ipc *ipc, struct ipc_torrent *tps, size_t ntps,
    enum ipc_tval *keys, size_t nkeys, tget_cb_t cb, void *arg)
{
    char *res;
    uint32_t rlen;
    enum ipc_err err;
    struct iobuf iob;

    if (nkeys == 0 || ntps == 0)
        return IPC_COMMERR;

    iob = iobuf_init(1 << 14);
    iobuf_swrite(&iob, "l4:tgetd4:froml");
    for (int i = 0; i < ntps; i++) {
        if (tps[i].by_hash) {
            iobuf_swrite(&iob, "20:");
            iobuf_write(&iob, tps[i].u.hash, 20);
        } else
            iobuf_print(&iob, "i%ue", tps[i].u.num);
    }
    iobuf_swrite(&iob, "e4:keysl");
    for (int k = 0; k < nkeys; k++)
        iobuf_print(&iob, "i%de", keys[k]);
    iobuf_swrite(&iob, "eee");

    if ((err = ipc_buf_req_res(ipc, &iob, &res, &rlen)) == 0)
        err = tget_common(res, keys, nkeys, cb, arg);
    return err;
}

enum ipc_err
btpd_tget_wc(struct ipc *ipc, enum ipc_twc twc, enum ipc_tval *keys,
    size_t nkeys, tget_cb_t cb, void *arg)
{
    char *res;
    uint32_t rlen;
    struct iobuf iob;
    enum ipc_err err;

    if (nkeys == 0)
        return IPC_COMMERR;

    iob = iobuf_init(1 << 14);
    iobuf_print(&iob, "l4:tgetd4:fromi%de4:keysl", twc);
    for (int i = 0; i < nkeys; i++)
        iobuf_print(&iob, "i%de", keys[i]);
    iobuf_swrite(&iob, "eee");

    if ((err = ipc_buf_req_res(ipc, &iob, &res, &rlen)) == 0)
        err = tget_common(res, keys, nkeys, cb, arg);
    return err;
}

enum ipc_err
btpd_add(struct ipc *ipc, const char *mi, size_t mi_size, const char *content,
    const char *name)
{
    struct iobuf iob = iobuf_init(1 << 10);
    iobuf_print(&iob, "l3:addd7:content%d:%s", (int)strlen(content),
        content);
    if (name != NULL)
        iobuf_print(&iob, "4:name%d:%s", (int)strlen(name), name);
    iobuf_print(&iob, "7:torrent%lu:", (unsigned long)mi_size);
    iobuf_write(&iob, mi, mi_size);
    iobuf_swrite(&iob, "ee");
    return ipc_buf_req_code(ipc, &iob);
}

static enum ipc_err
simple_treq(struct ipc *ipc, char *cmd, struct ipc_torrent *tp)
{
    struct iobuf iob = iobuf_init(32);
    if (tp->by_hash) {
        iobuf_print(&iob, "l%d:%s20:", (int)strlen(cmd), cmd);
        iobuf_write(&iob, tp->u.hash, 20);
        iobuf_swrite(&iob, "e");
    } else
        iobuf_print(&iob, "l%d:%si%uee", (int)strlen(cmd), cmd, tp->u.num);
    return ipc_buf_req_code(ipc, &iob);
}

enum ipc_err
btpd_del(struct ipc *ipc, struct ipc_torrent *tp)
{
    return simple_treq(ipc, "del", tp);
}

enum ipc_err
btpd_start(struct ipc *ipc, struct ipc_torrent *tp)
{
    return simple_treq(ipc, "start", tp);
}

enum ipc_err
btpd_stop(struct ipc *ipc, struct ipc_torrent *tp)
{
    return simple_treq(ipc, "stop", tp);
}

enum ipc_err
btpd_stop_all(struct ipc *ipc)
{
    struct iobuf iob = iobuf_init(16);
    iobuf_swrite(&iob, "l8:stop-alle");
    return ipc_buf_req_code(ipc, &iob);
}
