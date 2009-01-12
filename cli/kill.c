#include "btcli.h"

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
    enum ipc_err code;
    char *endptr;

    if (argc == 2) {
        seconds = strtol(argv[1], &endptr, 10);
        if (strlen(argv[1]) > endptr - argv[1] || seconds < 0)
            usage_kill();
    } else if (argc > 2)
        usage_kill();

    btpd_connect();
    if ((code = btpd_die(ipc, seconds)) != 0)
        diemsg("command failed (%s).\n", ipc_strerror(code));
}
