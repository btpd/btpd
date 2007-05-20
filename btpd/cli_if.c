#include "btpd.h"

#include <sys/un.h>
#include <iobuf.h>

struct cli {
    int sd;
    struct event read;
};

static struct event m_cli_incoming;

static int
write_buffer(struct cli *cli, struct io_buffer *iob)
{
    int err = 0;
    if (!iob->error) {
        uint32_t len = iob->off;
        write_fully(cli->sd, &len, sizeof(len));
        err = write_fully(cli->sd, iob->buf, iob->off);
    } else
        btpd_err("Out of memory.\n");
    buf_free(iob);
    return err;
}

static int
write_code_buffer(struct cli *cli, enum ipc_err code)
{
    struct io_buffer iob = buf_init(16);
    buf_print(&iob, "d4:codei%uee", code);
    return write_buffer(cli, &iob);
}

static int
write_add_buffer(struct cli *cli, unsigned num)
{
    struct io_buffer iob = buf_init(32);
    buf_print(&iob, "d4:codei%ue3:numi%uee", IPC_OK, num);
    return write_buffer(cli, &iob);
}

static void
write_ans(struct io_buffer *iob, struct tlib *tl, enum ipc_tval val)
{
    enum ipc_tstate ts = IPC_TSTATE_INACTIVE;
    switch (val) {
    case IPC_TVAL_CGOT:
        buf_print(iob, "i%dei%llde", IPC_TYPE_NUM,
            tl->tp == NULL ? tl->content_have : (long long)cm_content(tl->tp));
        return;
    case IPC_TVAL_CSIZE:
        buf_print(iob, "i%dei%llde", IPC_TYPE_NUM, tl->content_size);
        return;
    case IPC_TVAL_PCCOUNT:
        if (tl->tp == NULL)
            buf_print(iob, "i%dei%de", IPC_TYPE_ERR, IPC_ETINACTIVE);
        else
            buf_print(iob, "i%dei%lue", IPC_TYPE_NUM,
                (unsigned long)tl->tp->npieces);
        return;
    case IPC_TVAL_PCGOT:
        if (tl->tp == NULL)
            buf_print(iob, "i%dei%de", IPC_TYPE_ERR, IPC_ETINACTIVE);
        else
            buf_print(iob, "i%dei%lue", IPC_TYPE_NUM,
                (unsigned long)cm_pieces(tl->tp));
        return;
    case IPC_TVAL_PCSEEN:
        if (tl->tp == NULL)
            buf_print(iob, "i%dei%de", IPC_TYPE_NUM, 0);
        else {
            unsigned long pcseen = 0;
            for (unsigned long i = 0; i < tl->tp->npieces; i++)
                if (tl->tp->net->piece_count[i] > 0)
                    pcseen++;
            buf_print(iob, "i%dei%lue", IPC_TYPE_NUM, pcseen);
        }
        return;
    case IPC_TVAL_RATEDWN:
        buf_print(iob, "i%dei%lue", IPC_TYPE_NUM,
            tl->tp == NULL ? 0UL : tl->tp->net->rate_dwn / RATEHISTORY);
        return;
    case IPC_TVAL_RATEUP:
        buf_print(iob, "i%dei%lue", IPC_TYPE_NUM,
            tl->tp == NULL ? 0UL : tl->tp->net->rate_up / RATEHISTORY);
        return;
    case IPC_TVAL_SESSDWN:
        buf_print(iob, "i%dei%llde", IPC_TYPE_NUM,
            tl->tp == NULL ? 0LL : tl->tp->net->downloaded);
        return;
    case IPC_TVAL_SESSUP:
        buf_print(iob, "i%dei%llde", IPC_TYPE_NUM,
            tl->tp == NULL ? 0LL : tl->tp->net->uploaded);
        return;
    case IPC_TVAL_DIR:
        if (tl->dir != NULL)
            buf_print(iob, "i%de%d:%s", IPC_TYPE_STR, (int)strlen(tl->dir),
                tl->dir);
        else
            buf_print(iob, "i%dei%de", IPC_TYPE_ERR, IPC_EBADTENT);
        return;
    case IPC_TVAL_NAME:
        if (tl->name != NULL)
            buf_print(iob, "i%de%d:%s", IPC_TYPE_STR, (int)strlen(tl->name),
                tl->name);
        else
            buf_print(iob, "i%dei%de", IPC_TYPE_ERR, IPC_EBADTENT);
        return;
    case IPC_TVAL_IHASH:
        buf_print(iob, "i%de20:", IPC_TYPE_BIN);
        buf_write(iob, tl->hash, 20);
        return;
    case IPC_TVAL_NUM:
        buf_print(iob, "i%dei%ue", IPC_TYPE_NUM, tl->num);
        return;
    case IPC_TVAL_PCOUNT:
        buf_print(iob, "i%dei%ue", IPC_TYPE_NUM,
            tl->tp == NULL ? 0 : tl->tp->net->npeers);
        return;
    case IPC_TVAL_STATE:
        buf_print(iob, "i%de", IPC_TYPE_NUM);
        if (tl->tp != NULL) {
            switch (tl->tp->state) {
            case T_STARTING:
                ts = IPC_TSTATE_START;
                break;
            case T_STOPPING:
                ts = IPC_TSTATE_STOP;
                break;
            case T_SEED:
                ts = IPC_TSTATE_SEED;
                break;
            case T_LEECH:
                ts = IPC_TSTATE_LEECH;
                break;
            }
        }
        buf_print(iob, "i%de", ts);
        return;
    case IPC_TVAL_TOTDWN:
        buf_print(iob, "i%dei%llde", IPC_TYPE_NUM, tl->tot_down +
            (tl->tp == NULL ? 0 : tl->tp->net->downloaded));
        return;
    case IPC_TVAL_TOTUP:
        buf_print(iob, "i%dei%llde", IPC_TYPE_NUM, tl->tot_up +
            (tl->tp == NULL ? 0 : tl->tp->net->uploaded));
        return;
    case IPC_TVAL_TRERR:
        buf_print(iob, "i%dei%ue", IPC_TYPE_NUM,
            tl->tp == NULL ? 0 : tr_errors(tl->tp));
        return;
    case IPC_TVALCOUNT:
        break;
    }
    buf_print(iob, "i%dei%de", IPC_TYPE_ERR, IPC_ENOKEY);
}

static int
cmd_tget(struct cli *cli, int argc, const char *args)
{
    if (argc != 1 || !benc_isdct(args))
        return IPC_COMMERR;

    size_t nkeys;
    const char *keys, *p;
    enum ipc_tval *opts;
    struct io_buffer iob;

    if ((keys = benc_dget_lst(args, "keys")) == NULL)
        return IPC_COMMERR;

    nkeys = benc_nelems(keys);
    opts = btpd_calloc(nkeys, sizeof(*opts));

    p = benc_first(keys);
    for (int i = 0; i < nkeys; i++)
        opts[i] = benc_int(p, &p);

    iob = buf_init(1 << 15);
    buf_swrite(&iob, "d4:codei0e6:resultl");
    p = benc_dget_any(args, "from");
    if (benc_isint(p)) {
        enum ipc_twc from = benc_int(p, NULL);
        struct tlib *tlv[tlib_count()];
        tlib_put_all(tlv);
        for (int i = 0; i < sizeof(tlv) / sizeof(tlv[0]); i++) {
            if ((from == IPC_TWC_ALL ||
                    (tlv[i]->tp == NULL && from == IPC_TWC_INACTIVE) ||
                    (tlv[i]->tp != NULL && from == IPC_TWC_ACTIVE))) {
                buf_swrite(&iob, "l");
                for (int k = 0; k < nkeys; k++)
                    write_ans(&iob, tlv[i], opts[k]);
                buf_swrite(&iob, "e");
            }
        }
    } else if (benc_islst(p)) {
        for (p = benc_first(p); p != NULL; p = benc_next(p)) {
            struct tlib *tl = NULL;
            if (benc_isint(p))
                tl = tlib_by_num(benc_int(p, NULL));
            else if (benc_isstr(p) && benc_strlen(p) == 20)
                tl = tlib_by_hash(benc_mem(p, NULL, NULL));
            else {
                buf_free(&iob);
                free(opts);
                return IPC_COMMERR;
            }
            if (tl != NULL) {
                buf_swrite(&iob, "l");
                for (int i = 0; i < nkeys; i++)
                    write_ans(&iob, tl, opts[i]);
                buf_swrite(&iob, "e");
            } else
                buf_print(&iob, "i%de", IPC_ENOTENT);
        }
    }
    buf_swrite(&iob, "ee");
    free(opts);
    return write_buffer(cli, &iob);
}

static int
cmd_add(struct cli *cli, int argc, const char *args)
{
    if (argc != 1 || !benc_isdct(args))
        return IPC_COMMERR;

    struct tlib *tl;
    size_t mi_size = 0, csize = 0;
    const char *mi, *cp;
    char content[PATH_MAX];
    uint8_t hash[20];

    if ((mi = benc_dget_mem(args, "torrent", &mi_size)) == NULL)
        return IPC_COMMERR;

    if (!mi_test(mi, mi_size))
        return write_code_buffer(cli, IPC_EBADT);

    if ((cp = benc_dget_mem(args, "content", &csize)) == NULL ||
            csize >= PATH_MAX || csize == 0)
        return write_code_buffer(cli, IPC_EBADCDIR);

    if (cp[0] != '/')
        return write_code_buffer(cli, IPC_EBADCDIR);
    bcopy(cp, content, csize);
    content[csize] = '\0';

    tl = tlib_by_hash(mi_info_hash(mi, hash));
    if (tl != NULL)
        return write_code_buffer(cli, IPC_ETENTEXIST);
    tl = tlib_add(hash, mi, mi_size, content,
        benc_dget_str(args, "name", NULL));
    return write_add_buffer(cli, tl->num);
}

static int
cmd_del(struct cli *cli, int argc, const char *args)
{
    if (argc != 1)
        return IPC_COMMERR;

    int ret;
    struct tlib *tl;
    if (benc_isstr(args) && benc_strlen(args) == 20)
        tl = tlib_by_hash(benc_mem(args, NULL, NULL));
    else if (benc_isint(args))
        tl = tlib_by_num(benc_int(args, NULL));
    else
        return IPC_COMMERR;

    if (tl == NULL)
        ret = write_code_buffer(cli, IPC_ENOTENT);
    else {
        ret = write_code_buffer(cli, IPC_OK);
        if (tl->tp != NULL)
            torrent_stop(tl->tp, 1);
        else
            tlib_del(tl);
    }

    return ret;
}

static int
cmd_start(struct cli *cli, int argc, const char *args)
{
    if (argc != 1)
        return IPC_COMMERR;
    if (btpd_is_stopping())
        return write_code_buffer(cli, IPC_ESHUTDOWN);

    struct tlib *tl;
    enum ipc_err code = IPC_OK;
    if (benc_isstr(args) && benc_strlen(args) == 20)
        tl = tlib_by_hash(benc_mem(args, NULL, NULL));
    else if (benc_isint(args))
        tl = tlib_by_num(benc_int(args, NULL));
    else
        return IPC_COMMERR;

    if (tl == NULL)
        code = IPC_ENOTENT;
    else if (tl->tp != NULL)
        code = IPC_ETACTIVE;
    else
        if ((code = torrent_start(tl)) == IPC_OK)
            active_add(tl->hash);
    return write_code_buffer(cli, code);
}

static int
cmd_stop(struct cli *cli, int argc, const char *args)
{
    if (argc != 1)
        return IPC_COMMERR;

    struct tlib *tl;
    if (benc_isstr(args) && benc_strlen(args) == 20)
        tl = tlib_by_hash(benc_mem(args, NULL, NULL));
    else if (benc_isint(args))
        tl = tlib_by_num(benc_int(args, NULL));
    else
        return IPC_COMMERR;

    if (tl == NULL)
        return write_code_buffer(cli, IPC_ENOTENT);
    else if (tl->tp == NULL)
        return write_code_buffer(cli, IPC_ETINACTIVE);
    else  {
        // Stopping a torrent may trigger exit so we need to reply before.
        int ret = write_code_buffer(cli, IPC_OK);
        active_del(tl->hash);
        torrent_stop(tl->tp, 0);
        return ret;
    }
}

static int
cmd_stop_all(struct cli *cli, int argc, const char *args)
{
    struct torrent *tp;
    int ret = write_code_buffer(cli, IPC_OK);
    active_clear();
    BTPDQ_FOREACH(tp, torrent_get_all(), entry)
        if (tp->state != T_STOPPING)
            torrent_stop(tp, 0);
    return ret;
}

static int
cmd_die(struct cli *cli, int argc, const char *args)
{
    int err = write_code_buffer(cli, IPC_OK);
    if (!btpd_is_stopping()) {
        int grace_seconds = -1;
        if (argc == 1 && benc_isint(args))
            grace_seconds = benc_int(args, NULL);
        btpd_log(BTPD_L_BTPD, "Someone wants me dead.\n");
        btpd_shutdown(grace_seconds);
    }
    return err;
}

static struct {
    const char *name;
    int nlen;
    int (*fun)(struct cli *cli, int, const char *);
} cmd_table[] = {
    { "add",    3, cmd_add },
    { "del",    3, cmd_del },
    { "die",    3, cmd_die },
    { "start",  5, cmd_start },
    { "stop",   4, cmd_stop },
    { "stop-all", 8, cmd_stop_all},
    { "tget",   4, cmd_tget }
};

static int ncmds = sizeof(cmd_table) / sizeof(cmd_table[0]);

static int
cmd_dispatch(struct cli *cli, const char *buf)
{
    size_t cmdlen;
    const char *cmd;
    const char *args;

    cmd = benc_mem(benc_first(buf), &cmdlen, &args);

    for (int i = 0; i < ncmds; i++) {
        if ((cmdlen == cmd_table[i].nlen &&
                strncmp(cmd_table[i].name, cmd, cmdlen) == 0)) {
            return cmd_table[i].fun(cli, benc_nelems(buf) - 1, args);
        }
    }
    return ENOENT;
}

static void
cli_read_cb(int sd, short type, void *arg)
{
    struct cli *cli = arg;
    uint32_t cmdlen;
    uint8_t *msg = NULL;

    if (read_fully(sd, &cmdlen, sizeof(cmdlen)) != 0)
        goto error;

    msg = btpd_malloc(cmdlen);
    if (read_fully(sd, msg, cmdlen) != 0)
        goto error;

    if (!(benc_validate(msg, cmdlen) == 0 && benc_islst(msg) &&
            benc_first(msg) != NULL && benc_isstr(benc_first(msg))))
        goto error;

    if (cmd_dispatch(cli, msg) != 0)
        goto error;

    free(msg);
    btpd_ev_add(&cli->read, NULL);
    return;

error:
    close(cli->sd);
    free(cli);
    if (msg != NULL)
        free(msg);
}

void
client_connection_cb(int sd, short type, void *arg)
{
    int nsd;

    if ((nsd = accept(sd, NULL, NULL)) < 0) {
        if (errno == EWOULDBLOCK || errno == ECONNABORTED)
            return;
        else
            btpd_err("client accept: %s\n", strerror(errno));
    }

    if ((errno = set_blocking(nsd)) != 0)
        btpd_err("set_blocking: %s.\n", strerror(errno));

    struct cli *cli = btpd_calloc(1, sizeof(*cli));
    cli->sd = nsd;
    event_set(&cli->read, cli->sd, EV_READ, cli_read_cb, cli);
    btpd_ev_add(&cli->read, NULL);
}

void
ipc_init(void)
{
    int sd;
    struct sockaddr_un addr;
    size_t psiz = sizeof(addr.sun_path);

    addr.sun_family = PF_UNIX;
    if (snprintf(addr.sun_path, psiz, "%s/sock", btpd_dir) >= psiz)
        btpd_err("'%s/sock' is too long.\n", btpd_dir);

    if ((sd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
        btpd_err("sock: %s\n", strerror(errno));
    if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno == EADDRINUSE) {
            unlink(addr.sun_path);
            if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
                btpd_err("bind: %s\n", strerror(errno));
        } else
            btpd_err("bind: %s\n", strerror(errno));
    }

    if (chmod(addr.sun_path, ipcprot) == -1)
        btpd_err("chmod: %s (%s).\n", addr.sun_path, strerror(errno));
    listen(sd, 4);
    set_nonblocking(sd);

    event_set(&m_cli_incoming, sd, EV_READ | EV_PERSIST,
        client_connection_cb, NULL);
    btpd_ev_add(&m_cli_incoming, NULL);
}
