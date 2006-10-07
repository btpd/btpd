#include "btcli.h"

void
usage_del(void)
{
    printf(
        "Remove torrents from btpd.\n"
        "\n"
        "Usage: del torrent ...\n"
        "\n"
        "Arguments:\n"
        "torrent ...\n"
        "\tThe torrents to remove.\n"
        "\n");
    exit(1);
}

static struct option del_opts [] = {
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

void
cmd_del(int argc, char **argv)
{
    int ch;
    struct ipc_torrent t;

    while ((ch = getopt_long(argc, argv, "", del_opts, NULL)) != -1)
        usage_del();
    argc -= optind;
    argv += optind;

    if (argc < 1)
        usage_del();

    btpd_connect();
    for (int i = 0; i < argc; i++)
        if (torrent_spec(argv[i], &t))
            handle_ipc_res(btpd_del(ipc, &t), "del", argv[i]);
}
