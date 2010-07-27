#include "btcli.h"

void
usage_add(void)
{
    printf(
        "Add torrents to btpd.\n"
        "\n"
        "Usage: add [-n name] [-T] [-N] -d dir file(s)\n"
        "\n"
        "Arguments:\n"
        "file\n"
        "\tThe torrent file to add.\n"
        "\n"
        "Options:\n"
        "-d dir\n"
        "\tUse the dir for content.\n"
        "\n"
        "-n name\n"
        "\tSet the name displayed for this torrent.\n"
        "\n"
        "--nostart, -N\n"
        "\tDon't activate the torrent after adding it.\n"
        "\n"
        "--topdir, -T\n"
        "\tAppend the torrent top directory (if any) to the content path.\n"
        "\n"
        );
    exit(1);
}

static struct option add_opts [] = {
    { "help", no_argument, NULL, 'H' },
    { "nostart", no_argument, NULL, 'N'},
    { "topdir", no_argument, NULL, 'T'},
    {NULL, 0, NULL, 0}
};

void
cmd_add(int argc, char **argv)
{
    int ch, topdir = 0, start = 1, nfile, nloaded = 0;
    size_t dirlen = 0;
    char *dir = NULL, *name = NULL;

    while ((ch = getopt_long(argc, argv, "NTd:n:", add_opts, NULL)) != -1) {
        switch (ch) {
        case 'N':
            start = 0;
            break;
        case 'T':
            topdir = 1;
            break;
        case 'd':
            dir = optarg;
            if ((dirlen = strlen(dir)) == 0)
                diemsg("bad option value for -d.\n");
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

    if (argc < 1 || dir == NULL)
        usage_add();

    btpd_connect();
    char *mi;
    size_t mi_size;
    enum ipc_err code;
    char dpath[PATH_MAX];
    struct iobuf iob;

    for (nfile = 0; nfile < argc; nfile++) {
       if ((mi = mi_load(argv[nfile], &mi_size)) == NULL) {
           fprintf(stderr, "error loading '%s' (%s).\n", argv[nfile], strerror(errno));
           continue;
       }
       iob = iobuf_init(PATH_MAX);
       iobuf_write(&iob, dir, dirlen);
       if (topdir && !mi_simple(mi)) {
           size_t tdlen;
           const char *td =
               benc_dget_mem(benc_dget_dct(mi, "info"), "name", &tdlen);
           iobuf_swrite(&iob, "/");
           iobuf_write(&iob, td, tdlen);
       }
       iobuf_swrite(&iob, "\0");
       if ((errno = make_abs_path(iob.buf, dpath)) != 0) {
           fprintf(stderr, "make_abs_path '%s' failed (%s).\n", dpath, strerror(errno));
           iobuf_free(&iob);
           continue;
       }
       code = btpd_add(ipc, mi, mi_size, dpath, name);
       if ((code == IPC_OK) && start) {
           struct ipc_torrent tspec;
           tspec.by_hash = 1;
           mi_info_hash(mi, tspec.u.hash);
           code = btpd_start(ipc, &tspec);
       }
       if (code != IPC_OK) {
           fprintf(stderr, "command failed for '%s' (%s).\n", argv[nfile], ipc_strerror(code));
       } else {
           nloaded++;
       }
       iobuf_free(&iob);
    }

    if (nloaded != nfile) {
       diemsg("error loaded %d of %d files.\n", nloaded, nfile);
    }
}
