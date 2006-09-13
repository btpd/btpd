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
        "file ...\n"
        "\tThe torrents to remove.\n"
        "\n");
    exit(1);
}

void
cmd_del(int argc, char **argv)
{
    struct ipc_torrent t;

    if (argc < 2)
        usage_del();

    btpd_connect();
    for (int i = 1; i < argc; i++)
        if (torrent_spec(argv[i], &t))
            handle_ipc_res(btpd_del(ipc, &t), argv[i]);
}
