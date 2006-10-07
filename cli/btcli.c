#include "btcli.h"

const char *btpd_dir;
struct ipc *ipc;

void
btpd_connect(void)
{
    if ((errno = ipc_open(btpd_dir, &ipc)) != 0)
        err(1, "cannot open connection to btpd in %s", btpd_dir);
}

enum ipc_err
handle_ipc_res(enum ipc_err code, const char *cmd, const char *target)
{
    switch (code) {
    case IPC_OK:
        break;
    case IPC_COMMERR:
        errx(1, "%s", ipc_strerror(code));
    default:
        warnx("%s '%s': %s", cmd, target, ipc_strerror(code));
    }
    return code;
}

void
print_percent(long long part, long long whole)
{
    printf("%5.1f%% ", floor(1000.0 * part / whole) / 10);
}

void
print_rate(long long rate)
{
    if (rate >= 999.995 * (1 << 10))
        printf("%6.2fMB/s ", (double)rate / (1 << 20));
    else
        printf("%6.2fkB/s ", (double)rate / (1 << 10));
}

void
print_size(long long size)
{
    if (size >= 999.995 * (1 << 20))
        printf("%6.2fG ", (double)size / (1 << 30));
    else
        printf("%6.2fM ", (double)size / (1 << 20));
}
void
print_ratio(long long part, long long whole)
{
    printf("%7.2f ", (double)part / whole);
}

char
tstate_char(enum ipc_tstate ts)
{
    switch (ts) {
    case IPC_TSTATE_INACTIVE:
        return 'I';
    case IPC_TSTATE_START:
        return '+';
    case IPC_TSTATE_STOP:
        return '-';
    case IPC_TSTATE_LEECH:
        return 'L';
    case IPC_TSTATE_SEED:
        return 'S';
    }
    errx(1, "bad state");
}

int
torrent_spec(char *arg, struct ipc_torrent *tp)
{
    char *p;
    tp->u.num = strtoul(arg, &p, 10);
    if (*p == '\0') {
        tp->by_hash = 0;
        return 1;
    }
    if ((p = mi_load(arg, NULL)) == NULL) {
        warnx("bad torrent '%s' (%s)", arg, strerror(errno));
        return 0;
    }
    tp->by_hash = 1;
    mi_info_hash(p, tp->u.hash);
    free(p);
    return 1;
}

static struct {
    const char *name;
    void (*fun)(int, char **);
    void (*help)(void);
} cmd_table[] = {
    { "add", cmd_add, usage_add },
    { "del", cmd_del, usage_del },
    { "kill", cmd_kill, usage_kill },
    { "list", cmd_list, usage_list },
    { "start", cmd_start, usage_start },
    { "stop", cmd_stop, usage_stop },
    { "stat", cmd_stat, usage_stat }
};

int ncmds = sizeof(cmd_table) / sizeof(cmd_table[0]);

static void
usage(void)
{
    printf(
        "btcli is the btpd command line interface.\n"
        "\n"
        "Usage: btcli [main options] command [command options]\n"
        "\n"
        "Main options:\n"
        "-d dir\n"
        "\tThe btpd directory.\n"
        "\n"
        "--help [command]\n"
        "\tShow this text or help for the specified command.\n"
        "\n"
        "Commands:\n"
        "add\n"
        "del\n"
        "kill\n"
        "list\n"
        "start\n"
        "stat\n"
        "stop\n"
        "\n");
    exit(1);
}

static struct option base_opts [] = {
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

int
main(int argc, char **argv)
{
    int ch, help = 0;

    if (argc < 2)
        usage();

    while ((ch = getopt_long(argc, argv, "+d:", base_opts, NULL)) != -1) {
        switch (ch) {
        case 'd':
            btpd_dir = optarg;
            break;
        case 'H':
            help = 1;
            break;
        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc == 0)
        usage();

    if (btpd_dir == NULL)
        if ((btpd_dir = find_btpd_dir()) == NULL)
            errx(1, "cannot find the btpd directory");

    optind = 0;
    int found = 0;
    for (int i = 0; !found && i < ncmds; i++) {
        if (strcmp(argv[0], cmd_table[i].name) == 0) {
            found = 1;
            if (help)
                cmd_table[i].help();
            else
                cmd_table[i].fun(argc, argv);
        }
    }

    if (!found)
        usage();

    return 0;
}
