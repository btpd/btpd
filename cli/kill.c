#include "btcli.h"

void
usage_kill(void)
{
    printf(
        "Shutdown btpd.\n"
        "\n"
        "Usage: kill\n"
        "\n"
        );
    exit(1);
}

void
cmd_kill(int argc, char **argv)
{
    enum ipc_err code;

    if (argc > 1)
        usage_kill();

    btpd_connect();
    if ((code = btpd_die(ipc)) != 0)
        diemsg("command failed (%s).\n", ipc_strerror(code));
}
