#include <stdarg.h>

#include "btcli.h"
#include "utils.h"

const char *btpd_dir;
struct ipc *ipc;

void
diemsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

void
btpd_connect(void)
{
    if ((errno = ipc_open(btpd_dir, &ipc)) != 0)
        diemsg("cannot open connection to btpd in %s (%s).\n", btpd_dir,
            strerror(errno));
}

enum ipc_err
handle_ipc_res(enum ipc_err code, const char *cmd, const char *target)
{
    switch (code) {
    case IPC_OK:
        break;
    case IPC_COMMERR:
        diemsg("error in communication with btpd.\n");
    default:
        fprintf(stderr, "btcli %s '%s': %s.\n", cmd, target,
            ipc_strerror(code));
    }
    return code;
}

void
print_percent(long long part, long long whole)
{
    printf("%5.1f%% ", floor(1000.0 * part / whole) / 10);
}

char *bytestostr(double bytes, double prefix)
{
	int i;
	int cols;
	static char str[32];
	static const char iec[][4] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB" };
	static const char si[][3] = { "B", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" };
	const char *unit;
	char *fmt;

	if (siunits) {
		prefix = 1000.0;
		cols = LEN(si);
	} else {
		prefix = 1024.0;
		cols = LEN(iec);
	}

	for (i = 0; bytes >= prefix && i < cols; i++)
		bytes /= prefix;

	fmt = i ? "%.2f %s" : "%.0f %s";
	unit = siunits ? si[i] : iec[i];
	snprintf(str, sizeof(str), fmt, bytes, unit);

	return str;
}

void
print_rate(long long rate)
{
	printf("%s/s ", bytestostr(rate, (1 << 10)));
}

void
print_size(long long size)
{
	printf("%s ", bytestostr(size), (1 << 10));
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
    diemsg("unrecognized torrent state.\n");
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
        fprintf(stderr, "btcli: bad torrent '%s' (%s).\n", arg,
            strerror(errno));
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
    { "rate", cmd_rate, usage_rate },
    { "start", cmd_start, usage_start },
    { "stop", cmd_stop, usage_stop },
    { "stat", cmd_stat, usage_stat }
};

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
        "add\t- Add torrents to btpd.\n"
        "del\t- Remove torrents from btpd.\n"
        "kill\t- Shut down btpd.\n"
        "list\t- List torrents.\n"
        "rate\t- Set up/download rate limits.\n"
        "start\t- Activate torrents.\n"
        "stat\t- Display stats for active torrents.\n"
        "stop\t- Deactivate torrents.\n"
        "\n"
        "Note:\n"
        "Torrents can be specified either with its number or its file.\n"
        "\n"
        );
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
            diemsg("cannot find the btpd directory.\n");

    optind = 0;
    int found = 0;
    for (int i = 0; !found && i < ARRAY_COUNT(cmd_table); i++) {
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
