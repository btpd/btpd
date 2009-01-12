#include "btcli.h"

void
usage_stop(void)
{
    printf(
        "Deactivate torrents.\n"
        "\n"
        "Usage: stop -a\n"
        "       stop torrent ...\n"
        "\n"
        "Arguments:\n"
        "torrent ...\n"
        "\tThe torrents to deactivate.\n"
        "\n"
        "Options:\n"
        "-a\n"
        "\tDeactivate all active torrents.\n"
        "\n"
        );
    exit(1);
}

static struct option stop_opts [] = {
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

void
cmd_stop(int argc, char **argv)
{
    int ch, all = 0;
    struct ipc_torrent t;

    while ((ch = getopt_long(argc, argv, "a", stop_opts, NULL)) != -1) {
        switch (ch) {
        case 'a':
            all = 1;
            break;
        default:
            usage_stop();
        }
    }
    argc -= optind;
    argv += optind;

    if ((argc == 0 && !all) || (all && argc != 0))
        usage_stop();

    btpd_connect();
    if (all) {
        enum ipc_err code = btpd_stop_all(ipc);
        if (code != IPC_OK)
            diemsg("command failed (%s).\n", ipc_strerror(code));
    } else {
        for (int i = 0; i < argc; i++)
            if (torrent_spec(argv[i], &t))
                handle_ipc_res(btpd_stop(ipc, &t), "stop", argv[i]);
    }
}
