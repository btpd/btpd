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

#define buf_swrite(iob, s) buf_write(iob, s, sizeof(s) - 1)

static struct event m_cli_incoming;

static void
errdie(int error)
{
    if (error != 0)
        btpd_err("io_buf: %s.\n", strerror(error));
}

static void
cmd_stat(int argc, const char *args, FILE *fp)
{
    struct torrent *tp;
    struct io_buffer iob;
    errdie(buf_init(&iob, (1 << 14)));

    errdie(buf_swrite(&iob, "d"));
    errdie(buf_print(&iob, "6:npeersi%ue", net_npeers));
    errdie(buf_print(&iob, "9:ntorrentsi%ue", btpd_get_ntorrents()));
    errdie(buf_swrite(&iob, "8:torrentsl"));
    BTPDQ_FOREACH(tp, btpd_get_torrents(), entry) {
        uint32_t seen_npieces = 0;
        for (uint32_t i = 0; i < tp->meta.npieces; i++)
            if (tp->piece_count[i] > 0)
                seen_npieces++;
        errdie(buf_print(&iob, "d4:downi%jue", (intmax_t)tp->downloaded));
        errdie(buf_swrite(&iob, "4:hash20:"));
        errdie(buf_write(&iob, tp->meta.info_hash, 20));
        errdie(buf_print(&iob, "12:have npiecesi%ue", tp->have_npieces));
        errdie(buf_print(&iob, "6:npeersi%ue", tp->npeers));
        errdie(buf_print(&iob, "7:npiecesi%ue", tp->meta.npieces));
        errdie(buf_print(&iob, "4:path%d:%s",
                         (int)strlen(tp->relpath), tp->relpath));
        errdie(buf_print(&iob, "12:seen npiecesi%ue", seen_npieces));
        errdie(buf_print(&iob, "2:upi%juee", (intmax_t)tp->uploaded));
    }
    errdie(buf_swrite(&iob, "ee"));

    uint32_t len = iob.buf_off;
    fwrite(&len, sizeof(len), 1, fp);
    fwrite(iob.buf, 1, iob.buf_off, fp);
    free(iob.buf);
}

static void
cmd_add(int argc, const char *args, FILE *fp)
{
    struct io_buffer iob;
    errdie(buf_init(&iob, (1 << 10)));

    errdie(buf_write(&iob, "l", 1));
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
            errdie(buf_print(&iob, "d4:codei%dee", ENAMETOOLONG));
            continue;
        }

        bcopy(pathp, path, plen);
        path[plen] = '\0';
        btpd_log(BTPD_L_BTPD, "add request for %s.\n", path);
        errdie(buf_print(&iob, "d4:codei%dee", torrent_load(path)));
    }
    errdie(buf_write(&iob, "e", 1));

    uint32_t len = iob.buf_off;
    fwrite(&len, sizeof(len), 1, fp);
    fwrite(iob.buf, 1, iob.buf_off, fp);
    free(iob.buf);
}

static void
cmd_del(int argc, const char *args, FILE *fp)
{
    struct io_buffer iob;
    errdie(buf_init(&iob, (1 << 10)));

    errdie(buf_swrite(&iob, "l"));

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
            errdie(buf_swrite(&iob, "d4:codei0ee"));
        } else {
            btpd_log(BTPD_L_BTPD, "del request didn't match.\n");
            errdie(buf_print(&iob, "d4:codei%dee", ENOENT));
        }
    }
    errdie(buf_swrite(&iob, "e"));

    uint32_t len = iob.buf_off;
    fwrite(&len, sizeof(len), 1, fp);
    fwrite(iob.buf, 1, iob.buf_off, fp);
    free(iob.buf);
}

static void
cmd_die(int argc, const char *args, FILE *fp)
{
    char res[] = "d4:codei0ee";
    uint32_t len = sizeof(res) - 1;
    fwrite(&len, sizeof(len), 1, fp);
    fwrite(res, 1, len, fp);
    fflush(fp);
    btpd_log(BTPD_L_BTPD, "Someone wants me dead.\n");
    btpd_shutdown();
}

static struct {
    const char *name;
    int nlen;
    void (*fun)(int, const char *, FILE *);
} cmd_table[] = {
    { "add",    3, cmd_add },
    { "del",    3, cmd_del },
    { "die",    3, cmd_die },
    { "stat",   4, cmd_stat }
};

static int ncmds = sizeof(cmd_table) / sizeof(cmd_table[0]);

static void
cmd_dispatch(const char *buf, FILE *fp)
{
    size_t cmdlen;
    const char *cmd;
    const char *args;
    int found = 0;

    benc_str(benc_first(buf), &cmd, &cmdlen, &args);

    for (int i = 0; !found && i < ncmds; i++) {
        if (cmdlen == cmd_table[i].nlen &&
            strncmp(cmd_table[i].name, cmd, cmdlen) == 0) {
            cmd_table[i].fun(benc_nelems(buf) - 1, args, fp);
            found = 1;
        }
    }
}

static void
do_ipc(FILE *fp)
{
    uint32_t cmdlen, nread;
    char *buf;

    if (fread(&cmdlen, sizeof(cmdlen), 1, fp) != 1)
        return;

    buf = btpd_malloc(cmdlen);

    if ((nread = fread(buf, 1, cmdlen, fp)) == cmdlen) {
        if (benc_validate(buf, cmdlen) == 0 && benc_islst(buf) &&
            benc_first(buf) != NULL && benc_isstr(benc_first(buf)))
            cmd_dispatch(buf, fp);
    }

    free(buf);
}

void
client_connection_cb(int sd, short type, void *arg)
{
    int nsd;
    FILE *fp;

    if ((nsd = accept(sd, NULL, NULL)) < 0) {
        if (errno == EWOULDBLOCK || errno == ECONNABORTED)
            return;
        else
            btpd_err("client accept: %s\n", strerror(errno));
    }

    if ((errno = set_blocking(nsd)) != 0)
        btpd_err("set_blocking: %s.\n", strerror(errno));

    if ((fp = fdopen(nsd, "r+")) == NULL) {
        close(nsd);
        return;
    }

    do_ipc(fp);

    fclose(fp);
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
