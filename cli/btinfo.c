#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "metainfo.h"
#include "subr.h"

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

static void
print_metainfo(const char *mi)
{
    uint8_t hash[20];
    char hex[SHAHEXSIZE];
    char *name = mi_name(mi);
    unsigned nfiles = mi_nfiles(mi);
    struct mi_file *files = mi_files(mi);
    struct mi_announce *ann = mi_announce(mi);
    for (int i = 0; i < ann->ntiers; i++)
        for (int j = 0; j < ann->tiers[i].nurls; j++)
            printf("%d: %s\n", i, ann->tiers[i].urls[j]);
    printf("\n");
    mi_free_announce(ann);
    mi_info_hash(mi, hash);
    bin2hex(hash, hex, 20);
    printf("name: %s\n", name);
    printf("info hash: %s\n", hex);
    printf("length: %jd\n", (intmax_t)mi_total_length(mi));
    printf("piece length: %jd\n", (intmax_t)mi_piece_length(mi));
    printf("files: %u\n", nfiles);
    for (unsigned i = 0; i < nfiles; i++)
        printf("%s(%jd)\n", files[i].path, (intmax_t)files[i].length);
    free(name);
}

int
main(int argc, char **argv)
{
    int ch;

    srandom(time(NULL));
    while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1)
        usage();

    argc -= optind;
    argv += optind;

    if (argc < 1)
        usage();

    while (argc > 0) {
        char *mi = NULL;

        if ((mi = mi_load(*argv, NULL)) == NULL)
            err(1, "mi_load: %s", *argv);

        print_metainfo(mi);
        free(mi);

        argc--;
        argv++;
    }

    return 0;
}
