#include "btcli.h"

void
usage_stop(void)
{
    printf(
        "Stop torrents.\n"
        "\n"
        "Usage: stop torrent ...\n"
        "\n"
        );
    exit(1);
}

void
cmd_stop(int argc, char **argv)
{
    struct ipc_torrent t;

    if (argc < 2)
        usage_stop();

    btpd_connect();
    for (int i = 1; i < argc; i++)
        if (torrent_spec(argv[i], &t))
            handle_ipc_res(btpd_stop(ipc, &t), "stop", argv[i]);
}
