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
        "Options:\n"
        "-a\n"
        "\tActivate all inactive torrents.\n"
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
    int ch, all = 0;
    struct ipc_torrent t;

    while ((ch = getopt_long(argc, argv, "a", start_opts, NULL)) != -1) {
        switch (ch) {
        case 'a':
            all = 1;
            break;
        default:
            usage_start();
        }
    }
    argc -= optind;
    argv += optind;

    if ((argc == 0 && !all) || (all && argc != 0))
        usage_start();

    btpd_connect();
    if (all) {
        enum ipc_err code = btpd_start_all(ipc);
        if (code != IPC_OK)
            diemsg("command failed (%s).\n", ipc_strerror(code));
    } else {
       for (int i = 0; i < argc; i++)
           if (torrent_spec(argv[i], &t))
               handle_ipc_res(btpd_start(ipc, &t), "start", argv[i]);
    }
}
