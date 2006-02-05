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

#define buf_swrite(iob, s) buf_write(iob, s, sizeof(s) - 1)

static struct event m_cli_incoming;

static int
cmd_stat(struct cli *cli, int argc, const char *args)
{
    struct torrent *tp;
    struct io_buffer iob;
    buf_init(&iob, (1 << 14));

    buf_swrite(&iob, "d");
    buf_swrite(&iob, "4:codei0e");
    buf_print(&iob, "6:npeersi%ue", net_npeers);
    buf_print(&iob, "9:ntorrentsi%ue", btpd_get_ntorrents());
    buf_swrite(&iob, "8:torrentsl");
    BTPDQ_FOREACH(tp, btpd_get_torrents(), entry) {
        if (tp->state == T_ACTIVE) {
            uint32_t seen_npieces = 0;
            for (uint32_t i = 0; i < tp->meta.npieces; i++)
                if (tp->net->piece_count[i] > 0)
                    seen_npieces++;

            buf_print(&iob, "d4:downi%jue", (intmax_t)tp->net->downloaded);
            buf_print(&iob, "6:errorsi%ue", tr_errors(tp));
            buf_swrite(&iob, "4:hash20:");
            buf_write(&iob, tp->meta.info_hash, 20);
            buf_print(&iob, "4:havei%jde", (intmax_t)cm_get_size(tp));
            buf_print(&iob, "6:npeersi%ue", tp->net->npeers);
            buf_print(&iob, "7:npiecesi%ue", tp->meta.npieces);
            buf_print(&iob, "3:numi%ue", tp->num);
            buf_print(&iob, "4:path%d:%s", (int)strlen(tp->meta.name),
                tp->meta.name);
            buf_print(&iob, "2:rdi%lue", tp->net->rate_dwn);
            buf_print(&iob, "2:rui%lue", tp->net->rate_up);
            buf_print(&iob, "12:seen npiecesi%ue", seen_npieces);
            buf_swrite(&iob, "5:state1:A");
            buf_print(&iob, "5:totali%jde", (intmax_t)tp->meta.total_length);
            buf_print(&iob, "2:upi%juee", (intmax_t)tp->net->uploaded);
        } else {
            buf_swrite(&iob, "d4:hash20:");
            buf_write(&iob, tp->meta.info_hash, 20);
            buf_print(&iob, "3:numi%ue", tp->num);
            buf_print(&iob, "4:path%d:%s", (int)strlen(tp->meta.name),
                tp->meta.name);
            switch (tp->state) {
            case T_INACTIVE:
                buf_swrite(&iob, "5:state1:Ie");
                break;
            case T_STARTING:
                buf_swrite(&iob, "5:state1:Be");
                break;
            case T_STOPPING:
                buf_swrite(&iob, "5:state1:Ee");
                break;
            case T_ACTIVE:
                abort();
            }
        }
    }
    buf_swrite(&iob, "ee");

    uint32_t len = iob.buf_off;
    write_fully(cli->sd, &len, sizeof(len));
    write_fully(cli->sd, iob.buf, iob.buf_off);
    free(iob.buf);
    return 0;
}

#if 0
static void
cmd_add(int argc, const char *args, FILE *fp)
{
    struct io_buffer iob;
    buf_init(&iob, (1 << 10));

    buf_write(&iob, "l", 1);
    while (args != NULL) {
        size_t plen;
        char path[PATH_MAX];
        const char *pathp;

        if (!benc_isstr(args)) {
            free(iob.buf);
            return;
        }

        benc_str(args, &pathp, &plen, &args);

        if (plen >= PATH_MAX) {
            buf_print(&iob, "d4:codei%dee", ENAMETOOLONG);
            continue;
        }

        bcopy(pathp, path, plen);
        path[plen] = '\0';
        btpd_log(BTPD_L_BTPD, "add request for %s.\n", path);
        buf_print(&iob, "d4:codei%dee", torrent_load(path));
    }
    buf_write(&iob, "e", 1);

    uint32_t len = iob.buf_off;
    fwrite(&len, sizeof(len), 1, fp);
    fwrite(iob.buf, 1, iob.buf_off, fp);
    free(iob.buf);
}

static void
cmd_del(int argc, const char *args, FILE *fp)
{
    struct io_buffer iob;
    buf_init(&iob, (1 << 10));

    buf_swrite(&iob, "l");

    while (args != NULL) {
        size_t len;
        const char *hash;
        struct torrent *tp;

        if (!benc_isstr(args) ||
            benc_str(args, &hash, &len, &args) != 0 || len != 20) {
            free(iob.buf);
            return;
        }

        tp = btpd_get_torrent(hash);
        if (tp != NULL) {
            btpd_log(BTPD_L_BTPD, "del request for %s.\n", tp->relpath);
            torrent_unload(tp);
            buf_swrite(&iob, "d4:codei0ee");
        } else {
            btpd_log(BTPD_L_BTPD, "del request didn't match.\n");
            buf_print(&iob, "d4:codei%dee", ENOENT);
        }
    }
    buf_swrite(&iob, "e");

    uint32_t len = iob.buf_off;
    fwrite(&len, sizeof(len), 1, fp);
    fwrite(iob.buf, 1, iob.buf_off, fp);
    free(iob.buf);
}

#endif

static int
cmd_die(struct cli *cli, int argc, const char *args)
{
    char res[] = "d4:codei0ee";
    uint32_t len = sizeof(res) - 1;
    write_fully(cli->sd, &len, sizeof(len));
    write_fully(cli->sd, res, len);
    btpd_log(BTPD_L_BTPD, "Someone wants me dead.\n");
    btpd_shutdown((& (struct timeval) { 0, 0 }));
    return 0;
}

static int
cmd_start(struct cli *cli, int argc, const char *args)
{
    if (argc != 1 || !benc_isint(args))
        return EINVAL;

    int code;
    unsigned num;
    num = benc_int(args, NULL);
    struct torrent *tp = btpd_get_torrent_num(num);
    if (tp != NULL) {
        torrent_activate(tp);
        code = 0;
    } else
        code = 1;

    struct io_buffer iob;
    buf_init(&iob, 16);
    buf_print(&iob, "d4:codei%dee", code);
    uint32_t len = iob.buf_off;
    write_fully(cli->sd, &len, sizeof(len));
    write_fully(cli->sd, iob.buf, iob.buf_off);
    return 0;
}

static int
cmd_stop(struct cli *cli, int argc, const char *args)
{
    btpd_log(BTPD_L_BTPD, "%d\n", argc);
    if (argc != 1 || !benc_isint(args))
        return EINVAL;

    int code;
    unsigned num;
    num = benc_int(args, NULL);
    struct torrent *tp = btpd_get_torrent_num(num);
    if (tp != NULL) {
        torrent_deactivate(tp);
        code = 0;
    } else
        code = 1;

    struct io_buffer iob;
    buf_init(&iob, 16);
    buf_print(&iob, "d4:codei%dee", code);
    uint32_t len = iob.buf_off;
    write_fully(cli->sd, &len, sizeof(len));
    write_fully(cli->sd, iob.buf, iob.buf_off);
    return 0;
}

static struct {
    const char *name;
    int nlen;
    int (*fun)(struct cli *cli, int, const char *);
} cmd_table[] = {
#if 0
    { "add",    3, cmd_add },
    { "del",    3, cmd_del },
#endif
    { "die",    3, cmd_die },
    { "start",  5, cmd_start }, 
    { "stat",   4, cmd_stat },
    { "stop",   4, cmd_stop }
};

static int ncmds = sizeof(cmd_table) / sizeof(cmd_table[0]);

static int
cmd_dispatch(struct cli *cli, const char *buf)
{
    int err = 0;
    size_t cmdlen;
    const char *cmd;
    const char *args;

    cmd = benc_mem(benc_first(buf), &cmdlen, &args);

    btpd_log(BTPD_L_BTPD, "%.*s\n", (int)cmdlen, cmd);
    for (int i = 0; i < ncmds; i++) {
        if ((cmdlen == cmd_table[i].nlen &&
                strncmp(cmd_table[i].name, cmd, cmdlen) == 0)) {
            err = cmd_table[i].fun(cli, benc_nelems(buf) - 1, args);
            break;
        }
    }
    return err;
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

    event_add(&cli->read, NULL);
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
    event_add(&cli->read, NULL);
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
    event_add(&m_cli_incoming, NULL);
}
