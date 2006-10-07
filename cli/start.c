#include "btcli.h"

void
usage_start(void)
{
    printf(
        "Activate torrents.\n"
        "\n"
        "Usage: start torrent ...\n"
        "\n"
        "Arguments:\n"
        "torrent ...\n"
        "\tThe torrents to activate.\n"
        "\n"
        );
    exit(1);
}

static struct option start_opts [] = {
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

void
cmd_start(int argc, char **argv)
{
    int ch;
    struct ipc_torrent t;

    while ((ch = getopt_long(argc, argv, "", start_opts, NULL)) != -1)
        usage_start();
    argc -= optind;
    argv += optind;

    if (argc < 1)
        usage_start();

    btpd_connect();
    for (int i = 0; i < argc; i++)
        if (torrent_spec(argv[i], &t))
            handle_ipc_res(btpd_start(ipc, &t), "start", argv[i]);
}
