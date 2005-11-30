#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "metainfo.h"

static void
usage()
{
    fprintf(stderr, "Usage: btinfo file ...\n\n");
    exit(1);
}

static struct option longopts[] = {
    { "help", no_argument, NULL, 1 },
    { NULL, 0, NULL, 0 }
};

int
main(int argc, char **argv)
{
    int ch;

    while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1)
        usage();

    argc -= optind;
    argv += optind;

    if (argc < 1)
        usage();

    while (argc > 0) {
        struct metainfo *mi;

        if ((errno = load_metainfo(*argv, -1, 1, &mi)) != 0)
            err(1, "load_metainfo: %s", *argv);

        print_metainfo(mi);
        clear_metainfo(mi);
        free(mi);

        argc--;
        argv++;
    }

    return 0;
}
