#include "btcli.h"

void
usage_start(void)
{
    printf(
        "Start torrents.\n"
        "\n"
        "Usage: start torrent\n"
        "\n"
        );
    exit(1);
}

void
cmd_start(int argc, char **argv)
{
    struct ipc_torrent t;

    if (argc < 2)
        usage_start();

    btpd_connect();
    for (int i = 1; i < argc; i++)
        if (torrent_spec(argv[i], &t))
            handle_ipc_res(btpd_start(ipc, &t), "start", argv[i]);
}
