#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btpd_if.h"

static const char *btpd_dir = "/usr/btpd";
static struct ipc *ipc;

static void
handle_ipc_res(enum ipc_code code)
{
    switch (code) {
    case IPC_OK:
        return;
    case IPC_FAIL:
        warnx("Ipc failed.\n");
        break;
    case IPC_COMMERR:
        errx(1, "Communication error.\n");
    }
}

static void
btpd_connect(void)
{
    if ((errno = ipc_open(btpd_dir, &ipc)) != 0)
        errx(1, "Couldn't connect to btpd in %s (%s).\n",
            btpd_dir, strerror(errno));
}

void
usage_add(void)
{
    printf(
        "Add a torrent to btpd.\n"
        "\n"
        "Usage: add [-a] [-s] [-c dir] -f file\n"
        "\n"
        "Options:\n"
        "-a\n"
        "\tAppend the torrent top directory (if any) to the content path.\n"
        "\n"
        "-c dir\n"
        "\tThe directory where the content is (or will be downloaded to).\n"
        "\tDefault is the directory containing the torrent file.\n"
        "\n"
        "-f file\n"
        "\tThe torrent to add.\n"
        "\n"
        "-s\n"
        "\tStart the torrent.\n"
        "\n"
        );
    exit(1);
}

void
cmd_add(int argc, char **argv)
{
}

void
usage_del(void)
{
    printf(
        "Remove torrents from btpd.\n"
        "\n"
        "Usage: del num ...\n"
        "\n"
        "Arguments:\n"
        "num\n"
        "\tThe number of the torrent to remove.\n"
        "\n");
    exit(1);
}

void
cmd_del(int argc, char **argv)
{
    if (argc < 2)
        usage_del();

    unsigned nums[argc - 1];
    char *endptr;
    for (int i = 0; i < argc - 1; i++) {
        nums[i] = strtoul(argv[i + 1], &endptr, 10);
        if (strlen(argv[i + 1]) > endptr - argv[i + 1])
            usage_del();
    }
    btpd_connect();
    for (int i = 0; i < argc -1; i++)
        handle_ipc_res(btpd_del_num(ipc, nums[i]));
}

void
usage_kill(void)
{
    printf(
        "Shutdown btpd.\n"
        "\n"
        "Usage: kill [seconds]\n"
        "\n"
        "Arguments:\n"
        "seconds\n"
        "\tThe number of seconds btpd waits before giving up on unresponsive\n"
        "\ttrackers.\n"
        "\n"
        );
    exit(1);
}

void
cmd_kill(int argc, char **argv)
{
    int seconds = -1;
    char *endptr;
    if (argc == 1)
        ;
    else if (argc == 2) {
        seconds = strtol(argv[1], &endptr, 10);
        if (strlen(argv[1]) > endptr - argv[1] || seconds < 0)
            usage_kill();
    } else
        usage_kill();

    btpd_connect();
    btpd_die(ipc, seconds);
}

void
usage_list(void)
{
    printf(
        "List btpd's torrents.\n"
        "\n"
        "Usage: list\n"
        "\n"
        );
    exit(1);
}

void
cmd_list(int argc, char **argv)
{
    struct btstat *st;

    if (argc > 1)
        usage_list();

    btpd_connect();
    if ((errno = btpd_stat(ipc, &st)) != 0)
        err(1, "btpd_stat");
    for (int i = 0; i < st->ntorrents; i++)
        printf("%u. %s (%c)\n", st->torrents[i].num, st->torrents[i].name,
            st->torrents[i].state);
    printf("Listed %u torrent%s.\n", st->ntorrents,
        st->ntorrents == 1 ? "" : "s");
}

void
usage_stat(void)
{
    printf(
        "Display stats for active torrents.\n"
        "The stats displayed are:\n"
        "%% got, MB down, rate down. MB up, rate up\n"
        "peers, %% of pieces seen, tracker errors\n"
        "\n"
        "Usage: stat [-i] [-w seconds]\n"
        "\n"
        "Options:\n"
        "-i\n"
        "\tDisplay indivudal lines for each active torrent.\n"
        "\n"
        "-w n\n"
        "\tDisplay stats every n seconds.\n"
        "\n");
    exit(1);
}

void
print_stat(struct tpstat *cur)
{
    printf("%5.1f%% %6.1fM %7.2fkB/s %6.1fM %7.2fkB/s %4u %5.1f%%",
        100.0 * cur->have / cur->total,
        (double)cur->downloaded / (1 << 20),
        (double)cur->rate_down / (20 << 10),
        (double)cur->uploaded / (1 << 20),
        (double)cur->rate_up / (20 << 10),
        cur->npeers,
        100.0 * cur->nseen / cur->npieces
        );
    if (cur->errors > 0)
        printf(" E%u", cur->errors);
    printf("\n");
}

void
do_stat(int individual, int seconds)
{
    struct btstat *st;
    struct tpstat tot;
again:
    bzero(&tot, sizeof(tot));
    tot.num = -1;
    if ((errno = btpd_stat(ipc, &st)) != 0)
        err(1, "btpd_stat");
    for (int i = 0; i < st->ntorrents; i++) {
        struct tpstat *cur = &st->torrents[i];
        if (cur->state != 'A')
            continue;
        tot.uploaded += cur->uploaded;
        tot.downloaded += cur->downloaded;
        tot.rate_up += cur->rate_up;
        tot.rate_down += cur->rate_down;
        tot.npeers += cur->npeers;
        tot.nseen += cur->nseen;
        tot.npieces += cur->npieces;
        tot.have += cur->have;
        tot.total += cur->total;
        if (individual) {
            printf("%u. %s:\n", cur->num, cur->name);
            print_stat(cur);
        }
    }
    free_btstat(st);
    if (individual)
        printf("Total:\n");
    print_stat(&tot);
    if (seconds > 0) {
        sleep(seconds);
        goto again;
    }
}

static struct option stat_opts [] = {
    { "help", no_argument, NULL, 1 },
    {NULL, 0, NULL, 0}
};

void
cmd_stat(int argc, char **argv)
{
    int ch;
    int wflag = 0, iflag = 0, seconds = 0;
    char *endptr;
    while ((ch = getopt_long(argc, argv, "iw:", stat_opts, NULL)) != -1) {
        switch (ch) {
        case 'i':
            iflag = 1;
            break;
        case 'w':
            wflag = 1;
            seconds = strtol(optarg, &endptr, 10);
            if (strlen(optarg) > endptr - optarg || seconds < 1)
                usage_stat();
            break;
        default:
            usage_stat();
        }
    }
    argc -= optind;
    argv += optind;
    if (argc > 0)
        usage_stat();

    btpd_connect();
    do_stat(iflag, seconds);
}

void
usage_start(void)
{
    printf(
        "Activate torrents.\n"
        "\n"
        "Usage: start num ...\n"
        "\n"
        "Arguments:\n"
        "num\n"
        "\tThe number of the torrent to activate.\n"
        "\n");
    exit(1);
}

void
cmd_start(int argc, char **argv)
{
    if (argc < 2)
        usage_start();

    unsigned nums[argc - 1];
    char *endptr;
    for (int i = 0; i < argc - 1; i++) {
        nums[i] = strtoul(argv[i + 1], &endptr, 10);
        if (strlen(argv[i + 1]) > endptr - argv[i + 1])
            usage_start();
    }
    btpd_connect();
    for (int i = 0; i < argc -1; i++)
        handle_ipc_res(btpd_start_num(ipc, nums[i]));
}

void
usage_stop(void)
{
    printf(
        "Deactivate torrents.\n"
        "\n"
        "Usage: stop num ...\n"
        "\n"
        "Arguments:\n"
        "num\n"
        "\tThe number of the torrent to deactivate.\n"
        "\n");
    exit(1);
}

void
cmd_stop(int argc, char **argv)
{
    if (argc < 2)
        usage_stop();

    unsigned nums[argc - 1];
    char *endptr;
    for (int i = 0; i < argc - 1; i++) {
        nums[i] = strtoul(argv[i + 1], &endptr, 10);
        if (strlen(argv[i + 1]) > endptr - argv[i + 1])
            usage_stop();
    }
    btpd_connect();
    for (int i = 0; i < argc -1; i++)
        handle_ipc_res(btpd_stop_num(ipc, nums[i]));
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
    { "stat", cmd_stat, usage_stat },
    { "stop", cmd_stop, usage_stop }
};

static int ncmds = sizeof(cmd_table) / sizeof(cmd_table[0]);

void
usage(void)
{
    printf(
        "btcli is the btpd command line interface. Use this tool to interact\n"
        "with a btpd process.\n"
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
    { "help", no_argument, NULL, 1 },
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
        case 1:
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
