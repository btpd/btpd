#include <sys/types.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    printf("Name: %s\n", name);
    printf("Info hash: %s\n", bin2hex(mi_info_hash(mi, hash), hex, 20));
    printf("Tracker URLs: [ ");
    for (int i = 0; i < ann->ntiers; i++) {
        printf("[ ");
        for (int j = 0; j < ann->tiers[i].nurls; j++)
            printf("%s ", ann->tiers[i].urls[j]);
        printf("] ");
    }
    printf("]\n");
    printf("Number of pieces: %lu\n", (unsigned long)mi_npieces(mi));
    printf("Piece size: %lld\n", (long long)mi_piece_length(mi));
    printf("Total size: %lld\n", (long long)mi_total_length(mi));
    printf("Number of files: %u\n", nfiles);
    printf("Files:\n");
    for (int i = 0; i < nfiles; i++) {
        printf("%s (%lld)\n",
            files[i].path, (long long)files[i].length);
    }
    printf("\n");
    free(name);
    mi_free_files(nfiles, files);
    mi_free_announce(ann);
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

        if ((mi = mi_load(*argv, NULL)) == NULL) {
            fprintf(stderr, "failed to load torrent file '%s' (%s).\n",
                *argv, strerror(errno));
            exit(1);
        }

        print_metainfo(mi);
        free(mi);

        argc--;
        argv++;
    }

    return 0;
}
