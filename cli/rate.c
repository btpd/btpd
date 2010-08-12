#include "btcli.h"

void
usage_rate(void)
{
    printf(
        "Set upload and download rate.\n"
        "\n"
        "Usage: rate <up> <down>\n"
        "\n"
        "Arguments:\n"
        "<up> <down>\n"
        "\tThe up/down rate in KB/s\n"
        "\n"
        );
    exit(1);
}

static struct option start_opts [] = {
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

static unsigned
parse_rate(char *rate)
{
    unsigned out;
    char *end;

    out = strtol(rate, &end, 10);
    if (end == rate)
        usage_rate();

    if ((end[0] != '\0') && (end[1] != '\0'))
        usage_rate();

    switch(end[0]) {
        case 'g':
        case 'G':
            out <<= 30;
            break;
        case 'm':
        case 'M':
            out <<= 20;
            break;
        case '\0': /* default is 'k' */
        case 'k':
        case 'K':
            out <<= 10;
            break;
        case 'b':
        case 'B':
            break;
        default:
            usage_rate();
    }
    return out;
}

void
cmd_rate(int argc, char **argv)
{
    int ch;
    unsigned up, down;

    while ((ch = getopt_long(argc, argv, "", start_opts, NULL)) != -1)
        usage_rate();
    argc -= optind;
    argv += optind;

    if (argc < 2)
        usage_rate();

    up = parse_rate(argv[0]);
    down = parse_rate(argv[1]);

    btpd_connect();
    handle_ipc_res(btpd_rate(ipc, up, down), "rate", argv[1]);
}

