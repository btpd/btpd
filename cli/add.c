#include "btcli.h"

void
usage_add(void)
{
    printf(
        "Add torrents to btpd.\n"
        "\n"
        "Usage: add [--topdir] -d dir file\n"
        "       add file ...\n"
        "\n"
        "Arguments:\n"
        "file ...\n"
        "\tOne or more torrents to add.\n"
        "\n"
        "Options:\n"
        "-d dir\n"
        "\tUse the dir for content.\n"
        "\n"
        "--topdir\n"
        "\tAppend the torrent top directory (if any) to the content path.\n"
        "\tThis option cannot be used without the '-d' option.\n"
        "\n"
        );
    exit(1);
}

static struct option add_opts [] = {
    { "help", no_argument, NULL, 'H' },
    { "topdir", no_argument, NULL, 'T'},
    {NULL, 0, NULL, 0}
};

void
cmd_add(int argc, char **argv)
{
    int ch, topdir = 0;
    size_t dirlen = 0;
    char *dir = NULL, *name = NULL;

    while ((ch = getopt_long(argc, argv, "d:n:", add_opts, NULL)) != -1) {
        switch (ch) {
        case 'T':
            topdir = 1;
            break;
        case 'd':
            dir = optarg;
            if ((dirlen = strlen(dir)) == 0)
                errx(1, "bad option value for -d");
            break;
        case 'n':
            name = optarg;
            break;
        default:
            usage_add();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 1 || dir == NULL)
        usage_add();

    btpd_connect();
    char *mi;
    size_t mi_size;
    char dpath[PATH_MAX];
    struct io_buffer iob;

    if ((mi = mi_load(argv[0], &mi_size)) == NULL)
        err(1, "error loading '%s'", argv[0]);

    iob = buf_init(PATH_MAX);
    buf_write(&iob, dir, dirlen);
    if (topdir) {
        size_t tdlen;
        const char *td =
            benc_dget_mem(benc_dget_dct(mi, "info"), "name", &tdlen);
        buf_swrite(&iob, "/");
        buf_write(&iob, td, tdlen);
    }
    buf_swrite(&iob, "");
    if (realpath(iob.buf, dpath) == NULL)
        err(1, "realpath '%s'", dpath);
    handle_ipc_res(btpd_add(ipc, mi, mi_size, dpath, name), argv[0]);
    return;
}
