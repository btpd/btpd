#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "btpd.h"
#include "tracker_req.h"

struct cli {
    int sd;
    struct event read;
};

static struct event m_cli_incoming;

enum ipc_code { // XXX: Same as in cli/btpd_if.h
    IPC_OK,
    IPC_FAIL,
    IPC_ERROR,
    IPC_COMMERR
};

static int
write_buffer(struct cli *cli, struct io_buffer *iob)
{
    int err = 0;
    if (!iob->error) {
        uint32_t len = iob->buf_off;
        write_fully(cli->sd, &len, sizeof(len));
        err = write_fully(cli->sd, iob->buf, iob->buf_off);
    } else
        btpd_err("Out of memory.\n");
    if (iob->buf != NULL)
        free(iob->buf);
    return err;
}

static int
write_code_buffer(struct cli *cli, enum ipc_code code)
{
    struct io_buffer iob;
    buf_init(&iob, 16);
    buf_print(&iob, "d4:codei%uee", code);
    return write_buffer(cli, &iob);
}

static int
cmd_stat(struct cli *cli, int argc, const char *args)
{
    struct torrent *tp;
    struct io_buffer iob;
    buf_init(&iob, (1 << 14));

    buf_swrite(&iob, "d");
    buf_swrite(&iob, "4:codei0e");
    buf_print(&iob, "6:npeersi%ue", net_npeers);
    buf_print(&iob, "9:ntorrentsi%ue", torrent_count());
    buf_swrite(&iob, "8:torrentsl");
    BTPDQ_FOREACH(tp, torrent_get_all(), entry) {
        const char *name = torrent_name(tp);
        uint32_t seen_npieces = 0;
        for (uint32_t i = 0; i < tp->meta.npieces; i++)
            if (tp->net->piece_count[i] > 0)
                seen_npieces++;

        buf_swrite(&iob, "d");
        buf_print(&iob, "11:content goti%llde", (long long)cm_content(tp));
        buf_print(&iob, "12:content sizei%llde",
            (long long)tp->meta.total_length);
        buf_print(&iob, "10:downloadedi%llde", tp->net->downloaded);
        buf_swrite(&iob, "9:info hash20:");
        buf_write(&iob, tp->meta.info_hash, 20);
        buf_print(&iob, "4:name%d:%s", (int)strlen(name), name);
        buf_print(&iob, "5:peersi%ue", tp->net->npeers);
        buf_print(&iob, "10:pieces goti%ue", cm_pieces(tp));
        buf_print(&iob, "11:pieces seeni%ue", seen_npieces);
        buf_print(&iob, "9:rate downi%lue", tp->net->rate_dwn);
        buf_print(&iob, "7:rate upi%lue", tp->net->rate_up);
        buf_print(&iob, "5:statei%ue", tp->state);
        buf_print(&iob, "14:torrent piecesi%ue", tp->meta.npieces);
        buf_print(&iob, "14:tracker errorsi%ue", tr_errors(tp));
        buf_print(&iob, "8:uploadedi%llde", tp->net->uploaded);
        buf_swrite(&iob, "e");
    }
    buf_swrite(&iob, "ee");
    return write_buffer(cli, &iob);
}

static int
cmd_add(struct cli *cli, int argc, const char *args)
{
    if (argc != 1)
        return EINVAL;
    if (btpd_is_stopping())
        return write_code_buffer(cli, IPC_FAIL);

    size_t hlen;
    struct torrent *tp;
    enum ipc_code code = IPC_OK;
    const uint8_t *hash = benc_dget_mem(args, "hash", &hlen);
    char *content = benc_dget_str(args, "content", NULL);
    char *torrent = benc_dget_str(args, "torrent", NULL);

    if (!(hlen == 20 && content != NULL && torrent != NULL)) {
        code = IPC_COMMERR;
        goto out;
    }
    if ((tp = torrent_get(hash)) != NULL) {
        code = tp->state == T_STOPPING ? IPC_FAIL : IPC_OK;
        goto out;
    }
    if (torrent_set_links(hash, torrent, content) != 0) {
        code = IPC_ERROR;
        goto out;
    }
    if (torrent_start(hash) != 0)
        code = IPC_ERROR;

out:
    if (content != NULL)
        free(content);
    if (torrent != NULL)
        free(torrent);

    if (code == IPC_COMMERR)
        return EINVAL;
    else
        return write_code_buffer(cli, code);
}

static int
cmd_del(struct cli *cli, int argc, const char *args)
{
    if (argc != 1 || !benc_isstr(args))
        return EINVAL;

    size_t hlen;
    uint8_t *hash = (uint8_t *)benc_mem(args, &hlen, NULL);
    if (hlen != 20)
        return EINVAL;
    // Stopping a torrent may trigger exit so we need to reply before.
    int ret = write_code_buffer(cli, IPC_OK);
    struct torrent *tp = torrent_get(hash);
    if (tp != NULL)
        torrent_stop(tp);
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
    { "stat",   4, cmd_stat }
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

    if (chmod(addr.sun_path, 0600) == -1)
        btpd_err("chmod: %s (%s).\n", addr.sun_path, strerror(errno));
    listen(sd, 4);
    set_nonblocking(sd);

    event_set(&m_cli_incoming, sd, EV_READ | EV_PERSIST,
        client_connection_cb, NULL);
    btpd_ev_add(&m_cli_incoming, NULL);
}
