#include <sys/types.h>
#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "benc.h"
#include "metainfo.h"
#include "stream.h"
#include "subr.h"
#include "btpd_if.h"

static void
usage()
{
    printf("Usage: btcli command [options] [files]\n"
	   "Commands:\n"
	   "add <file_1> ... [file_n]\n"
	   "\tAdd the given torrents to btpd.\n"
	   "\n"
	   "del <file_1> ... [file_n]\n"
	   "\tRemove the given torrents from btpd.\n"
	   "\n"
	   "die\n"
	   "\tShut down btpd.\n"
	   "\n"
           "list\n"
           "\tList active torrents.\n"
           "\n"
           "stat [-i] [-w n] [file_1] ... [file_n]\n"
           "\tShow stats for either all active or the given torrents.\n"
           "\tThe stats displayed are:\n"
           "\t%% of pieces seen, %% of pieces verified, \n"
           "\tMB down, rate down, MB up, rate up, no peers\n"
           "-i\n"
           "\tShow stats per torrent in addition to total stats.\n"
           "-w n\n"
           "\tRepeat every n seconds.\n"
           "\n"
	   "Common options:\n"
	   "--ipc key\n"
	   "\tTalk to the btpd started with the same key.\n"
	   "\n"
	   "--help\n"
	   "\tShow this help.\n"
	   "\n");
    exit(1);
}

static void
handle_error(int error)
{
    switch (error) {
    case 0:
	break;
    case ENOENT:
    case ECONNREFUSED:
	errx(1, "Couldn't connect. Check that btpd is running.");
    default:
	errx(1, "%s", strerror(error));
    }
}

static void
do_ipc_open(char *ipctok, struct ipc **ipc)
{
    switch (ipc_open(ipctok, ipc)) {
    case 0:
	break;
    case EINVAL:
	errx(1, "--ipc argument only takes letters and digits.");
    case ENAMETOOLONG:
	errx(1, "--ipc argument is too long.");
    }
}

struct cb {
    char *path;
    uint8_t *piece_field;
    uint32_t have;
    struct metainfo *meta;
};

static void
hash_cb(uint32_t index, uint8_t *hash, void *arg)
{
    struct cb *cb = arg;
    if (hash != NULL)
	if (bcmp(hash, cb->meta->piece_hash[index], SHA_DIGEST_LENGTH) == 0) {
	    set_bit(cb->piece_field, index);
            cb->have++;
	}
    printf("\rTested: %5.1f%%", 100.0 * (index + 1) / cb->meta->npieces);
    fflush(stdout);
}

static int
fd_cb(const char *path, int *fd, void *arg)
{
    struct cb *fp = arg;
    return vopen(fd, O_RDONLY, "%s.d/%s", fp->path, path);
}

static void
gen_ifile(char *path)
{
    int fd;
    struct cb cb;
    struct metainfo *mi;
    size_t field_len;

    if ((errno = load_metainfo(path, -1, 1, &mi)) != 0)
	err(1, "load_metainfo: %s", path);

    field_len = ceil(mi->npieces / 8.0);
    cb.path = path;
    cb.piece_field = calloc(1, field_len);
    cb.have = 0;
    cb.meta = mi;

    if (cb.piece_field == NULL)
	errx(1, "Out of memory.\n");

    if ((errno = bts_hashes(mi, fd_cb, hash_cb, &cb)) != 0)
	err(1, "bts_hashes");
    printf("\nHave: %5.1f%%\n", 100.0 * cb.have / cb.meta->npieces);

    if ((errno = vopen(&fd, O_WRONLY|O_CREAT, "%s.i", path)) != 0)
	err(1, "opening %s.i", path);

    if (ftruncate(fd, field_len + mi->npieces *
	    (off_t)ceil(mi->piece_length / (double)(1 << 17))) < 0)
	err(1, "ftruncate: %s", path);

    if (write(fd, cb.piece_field, field_len) != field_len)
	err(1, "write %s.i", path);

    if (close(fd) < 0)
	err(1, "close %s.i", path);

    clear_metainfo(mi);
    free(mi);
}

static struct option add_opts[] = {
    { "ipc", required_argument, NULL, 1 },
    { "help", required_argument, NULL, 2},
    {NULL, 0, NULL, 0}
};

static void
do_add(char *ipctok, char **paths, int npaths, char **out)
{
    struct ipc *ipc;
    do_ipc_open(ipctok, &ipc);
    handle_error(btpd_add(ipc, paths, npaths, out));
    ipc_close(ipc);
}

static void
cmd_add(int argc, char **argv)
{
    int ch;
    char *ipctok = NULL;
    while ((ch = getopt_long(argc, argv, "", add_opts, NULL)) != -1) {
	switch(ch) {
	case 1:
	    ipctok = optarg;
	    break;
	default:
	    usage();
	}
    }
    argc -= optind;
    argv += optind;

    if (argc < 1)
	usage();

    for (int i = 0; i < argc; i++) {
	int64_t code;
	char *res;
	int fd;
	char *path;
	errno = vopen(&fd, O_RDONLY, "%s.i", argv[i]);
	if (errno == ENOENT) {
	    printf("Testing %s for content.\n", argv[i]);
	    gen_ifile(argv[i]);
	} else if (errno != 0)
	    err(1, "open %s.i", argv[i]);
	else
	    close(fd);

	if ((errno = canon_path(argv[i], &path)) != 0)
	    err(1, "canon_path");
	do_add(ipctok, &path, 1, &res);
	free(path);
	benc_dget_int64(benc_first(res), "code", &code);
	if (code == EEXIST)
	    printf("btpd already had %s.\n", argv[i]);
	else if (code != 0) {
	    printf("btpd indicates error: %s for %s.\n",
	        strerror(code), argv[i]);
	}
        free(res);
    }
}

static struct option del_opts[] = {
    { "ipc", required_argument, NULL, 1 },
    { "help", required_argument, NULL, 2},
    {NULL, 0, NULL, 0}
};

static void
do_del(char *ipctok, uint8_t (*hashes)[20], int nhashes, char **out)
{
    struct ipc *ipc;
    do_ipc_open(ipctok, &ipc);
    handle_error(btpd_del(ipc, hashes, nhashes, out));
    ipc_close(ipc);
}

static void
cmd_del(int argc, char **argv)
{
    int ch;
    char *ipctok = NULL;
    while ((ch = getopt_long(argc, argv, "", del_opts, NULL)) != -1) {
	switch(ch) {
	case 1:
	    ipctok = optarg;
	    break;
	default:
	    usage();
	}
    }
    argc -= optind;
    argv += optind;

    if (argc < 1)
	usage();

    uint8_t hashes[argc][20];
    char *res;
    const char *d;

    for (int i = 0; i < argc; i++) {
	struct metainfo *mi;
	if ((errno = load_metainfo(argv[i], -1, 0, &mi)) != 0)
	    err(1, "load_metainfo: %s", argv[i]);
	bcopy(mi->info_hash, hashes[i], 20);
	clear_metainfo(mi);
	free(mi);	
    }
    
    do_del(ipctok, hashes, argc, &res);
    d = benc_first(res);
    for (int i = 0; i < argc; i++) {
	int64_t code;
	benc_dget_int64(d, "code", &code);
	if (code == ENOENT)
	    printf("btpd didn't have %s.\n", argv[i]);
	else if (code != 0) {
	    printf("btpd indicates error: %s for %s.\n",
		   strerror(code), argv[i]);
	}
	d = benc_next(d);
    }
    free(res);
}

static struct option die_opts[] = {
    { "ipc", required_argument, NULL, 1 },
    { "help", no_argument, NULL, 2 },
    {NULL, 0, NULL, 0}
};

static void
do_die(char *ipctok)
{
    struct ipc *ipc;
    do_ipc_open(ipctok, &ipc);
    handle_error(btpd_die(ipc));
    ipc_close(ipc);
}

static void
cmd_die(int argc, char **argv)
{
    int ch;
    char *ipctok = NULL;

    while ((ch = getopt_long(argc, argv, "", die_opts, NULL)) != -1) {
	switch (ch) {
	case 1:
	    ipctok = optarg;
	    break;
	default:
	    usage();
	}
    }
    do_die(ipctok);
}

static struct option stat_opts[] = {
    { "ipc", required_argument, NULL, 1 },
    { "help", no_argument, NULL, 2 },
    {NULL, 0, NULL, 0}
};

static void
do_stat(char *ipctok, char **out)
{
    struct ipc *ipc;
    do_ipc_open(ipctok, &ipc);
    handle_error(btpd_stat(ipc, out));
    ipc_close(ipc);
}

struct tor {
    char *path;
    uint8_t hash[20];
    uint64_t down;
    uint64_t up;
    uint64_t npeers;
    uint64_t npieces;
    uint64_t have_npieces;
    uint64_t seen_npieces;
};

struct tor **parse_tors(char *res, uint8_t (*hashes)[20], int nhashes)
{
    struct tor **tors;
    int64_t num;
    const char *p;
    benc_dget_int64(res, "ntorrents", &num);
    benc_dget_lst(res, "torrents", &p);

    tors = calloc(sizeof(*tors), num + 1);
    int i = 0;
    for (p = benc_first(p); p; p = benc_next(p)) {
        int j;
        const char *hash;
        benc_dget_str(p, "hash", &hash, NULL);

        for (j = 0; j < nhashes; j++) {
            if (bcmp(hashes[i], hash, 20) == 0)
                break;
        }
        if (j < nhashes || nhashes == 0) {
            tors[i] = calloc(sizeof(*tors[i]), 1);
            bcopy(hash, tors[i]->hash, 20);
            benc_dget_int64(p, "down", &tors[i]->down);
            benc_dget_int64(p, "up", &tors[i]->up);
            benc_dget_int64(p, "npeers", &tors[i]->npeers);
            benc_dget_int64(p, "npieces", &tors[i]->npieces);
            benc_dget_int64(p, "have npieces", &tors[i]->have_npieces);
            benc_dget_int64(p, "seen npieces", &tors[i]->seen_npieces);
            benc_dget_strz(p, "path", &tors[i]->path, NULL);
            i++;
        }
    }
    return tors;
}

static void
free_tors(struct tor **tors)
{
    for (int i = 0; tors[i] != NULL; i++) {
        free(tors[i]->path);
        free(tors[i]);
    }
    free(tors);
}

static void
print_stat(struct tor *cur, struct tor *old, double ds)
{
    if (old == NULL) {
        printf("%5.1f%% %5.1f%% %6.1fM      - kB/s %6.1fM      - kB/s %4u\n",
               100 * cur->seen_npieces / (double)cur->npieces,
               100 * cur->have_npieces / (double)cur->npieces,
               cur->down / (double)(1 << 20),
               cur->up / (double)(1 << 20),
               (unsigned)cur->npeers);
    } else {
        printf("%5.1f%% %5.1f%% %6.1fM %7.2fkB/s %6.1fM %7.2fkB/s %4u\n",
               100 * cur->seen_npieces / (double)cur->npieces,
               100 * cur->have_npieces / (double)cur->npieces,
               cur->down / (double)(1 << 20),
               (cur->down - old->down) / ds / (1 << 10),
               cur->up / (double)(1 << 20),
               (cur->up - old->up) / ds / (1 << 10),
               (unsigned)cur->npeers
            );
    }
}

static void
grok_stat(char *ipctok, int iflag, int wait,
          uint8_t (*hashes)[20], int nhashes)
{
    int i, j;
    char *res;
    struct tor **cur, **old = NULL;
    struct tor curtot, oldtot;
    struct timeval tv_cur, tv_old;
    double ds;
again:
    do_stat(ipctok, &res);
    gettimeofday(&tv_cur, NULL);
    if (old == NULL) 
	ds = wait;
    else {
	struct timeval delta;
	timersub(&tv_old, &tv_cur, &delta);
	ds = delta.tv_sec + delta.tv_usec / 1000000.0;
	if (ds < 0)
	    ds = wait;
    }
    tv_old = tv_cur;
    cur = parse_tors(res, hashes, nhashes);
    free(res);

    if (iflag) {
        for (i = 0; cur[i] != NULL; i++) {
            if (old == NULL) {
                printf("%s:\n", rindex(cur[i]->path, '/') + 1);
                print_stat(cur[i], NULL, ds);
            } else {
                for (j = 0; old[j] != NULL; j++)
                    if (bcmp(cur[i]->hash, old[j]->hash, 20) == 0)
                        break;
                printf("%s:\n", rindex(cur[i]->path, '/') + 1);
                print_stat(cur[i], old[j], ds);
            }
        }
    }

    bzero(&curtot, sizeof(curtot));
    for (i = 0; cur[i] != NULL; i++) {
        curtot.down += cur[i]->down;
        curtot.up += cur[i]->up;
        curtot.npeers += cur[i]->npeers;
        curtot.npieces += cur[i]->npieces;
        curtot.have_npieces += cur[i]->have_npieces;
        curtot.seen_npieces += cur[i]->seen_npieces;
    }
    if (iflag)
        printf("Total:\n");
    if (old != NULL)
        print_stat(&curtot, &oldtot, ds);
    else
        print_stat(&curtot, NULL, ds);

    if (wait) {
        if (old != NULL)
            free_tors(old);
        old = cur;
        oldtot = curtot;
        sleep(wait);
        goto again;
    }
    free_tors(cur);
}

static void
cmd_stat(int argc, char **argv)
{
    int ch;
    char *ipctok = NULL;
    int wait = 0;
    int iflag = 0;

    while ((ch = getopt_long(argc, argv, "iw:", stat_opts, NULL)) != -1) {
	switch (ch) {
        case 'i':
            iflag = 1;
            break;
        case 'w':
            wait = atoi(optarg);
            if (wait <= 0)
                errx(1, "-w argument must be an integer > 0.");
            break;
	case 1:
	    ipctok = optarg;
	    break;
	default:
	    usage();
	}
    }
    argc -= optind;
    argv += optind;

    if (argc > 0) {
        uint8_t hashes[argc][20];
        for (int i = 0; i < argc; i++) {
            struct metainfo *mi;
            if ((errno = load_metainfo(argv[i], -1, 0, &mi)) != 0)
                err(1, "load_metainfo: %s", argv[i]);
            bcopy(mi->info_hash, hashes[i], 20);
            clear_metainfo(mi);
            free(mi);
        }
        grok_stat(ipctok, iflag, wait, hashes, argc);
    } else
        grok_stat(ipctok, iflag, wait, NULL, 0);
}

static struct option list_opts[] = {
    { "ipc", required_argument, NULL, 1 },
    { "help", no_argument, NULL, 2 },
    {NULL, 0, NULL, 0}
};

static void
cmd_list(int argc, char **argv)
{
    int ch;
    char *ipctok = NULL;

    while ((ch = getopt_long(argc, argv, "", list_opts, NULL)) != -1) {
	switch (ch) {
	case 1:
	    ipctok = optarg;
	    break;
	default:
	    usage();
	}
    }
    char *res;
    const char *p;
    char *path;
    do_stat(ipctok, &res);

    benc_dget_lst(res, "torrents", &p);
    int count = 0;
    for (p = benc_first(p); p; p = benc_next(p)) {
        count++;
        benc_dget_strz(p, "path", &path, NULL);
        printf("%s\n", path);
        free(path);
    }
    printf("%d torrent%s.\n", count, count == 1 ? "" : "s");
}

static struct {
    const char *name;
    void (*fun)(int, char **);
} cmd_table[] = {
    { "add", cmd_add },
    { "del", cmd_del },
    { "die", cmd_die },
    { "list", cmd_list},
    { "stat", cmd_stat }
};

static int ncmds = sizeof(cmd_table) / sizeof(cmd_table[0]);

int
main(int argc, char **argv)
{
    if (argc < 2)
	usage();
    
    int found = 0;
    for (int i = 0; !found && i < ncmds; i++) {
	if (strcmp(argv[1], cmd_table[i].name) == 0) {
	    found = 1;
	    cmd_table[i].fun(argc - 1, argv + 1);
	}
    }

    if (!found)
	usage();

    return 0;
}
