#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btpd.h"

static void
writepid(int pidfd)
{
    FILE *fp = fdopen(dup(pidfd), "w");
    fprintf(fp, "%d", getpid());
    fclose(fp);
}

static char *
find_homedir(void)
{
    char *res = getenv("BTPD_HOME");
    if (res == NULL) {
	char *home = getenv("HOME");
	if (home == NULL) {
	    struct passwd *pwent = getpwuid(getuid());
	    if (pwent == NULL)
		errx(1, "Can't find my home directory.\n");
	    home = pwent->pw_dir;
	    endpwent();
	}
	asprintf(&res, "%s/.btpd", home);
    }
    return res;
}

static void
setup_daemon(const char *dir)
{
    int pidfd;

    if (dir == NULL)
	dir = find_homedir();

    btpd_dir = dir;

    if (mkdir(dir, 0777) == -1 && errno != EEXIST)
        err(1, "Couldn't create home '%s'", dir);

    if (chdir(dir) != 0)
        err(1, "Couldn't change working directory to '%s'", dir);

    pidfd = open("pid", O_CREAT|O_WRONLY|O_NONBLOCK|O_EXLOCK, 0666);
    if (pidfd == -1)
        err(1, "Couldn't open 'pid'");

    if (btpd_daemon) {
        if (daemon(1, 1) != 0)
            err(1, "Failed to daemonize");
        freopen("/dev/null", "r", stdin);
        if (freopen("log", "a", stdout) == NULL)
            err(1, "Couldn't open 'log'");
        dup2(fileno(stdout), fileno(stderr));
        setlinebuf(stdout);
        setlinebuf(stderr);
    }

    writepid(pidfd);
}

static void
usage(void)
{
    printf("Usage: btpd [options]\n"
	"\n"
	"Options:\n"
	"\n"
	"--bw-hz n\n"
	"\tRun the bandwidth limiter at n hz.\n"
	"\tDefault is 8 hz.\n"
	"\n"
	"--bw-in n\n"
	"\tLimit incoming BitTorrent traffic to n kB/s.\n"
	"\tDefault is 0 which means unlimited.\n"
	"\n"
	"--bw-out n\n"
	"\tLimit outgoing BitTorrent traffic to n kB/s.\n"
	"\tDefault is 0 which means unlimited.\n"
	"\n"
	"-d\n"
	"\tKeep the btpd process in the foregorund and log to std{out,err}.\n"
	"\tThis option is intended for debugging purposes.\n"
	"\n"
	"--ipc key\n"
	"\tThe same key must be used by the cli to talk to this\n"
	"\tbtpd instance. You shouldn't need to use this option.\n"
	"\n"
	"--logfile file\n"
	"\tLog to the given file. By default btpd logs to ./btpd.log.\n"
	"\n"
	"-p n, --port n\n"
	"\tListen at port n. Default is 6881.\n"
	"\n"
	"--help\n"
	"\tShow this help.\n"
	"\n");
    exit(1);
}

static int longval = 0;

static struct option longopts[] = {
    { "port",	required_argument,	NULL,		'p' },
    { "bw-hz",	required_argument,	&longval,	6 },
    { "bw-in",	required_argument,	&longval,	1 },
    { "bw-out",	required_argument,	&longval,	2 },
    { "help",	no_argument,		&longval,	5 },
    { NULL,	0,			NULL,		0 }
};

int
main(int argc, char **argv)
{
    char *dir = NULL;

    setlocale(LC_ALL, "");

    for (;;) {
	switch (getopt_long(argc, argv, "dp:", longopts, NULL)) {
	case -1:
	    goto args_done;
	case 'd':
	    btpd_daemon = 0;
	    break;
	case 'p':
	    net_port = atoi(optarg);
	    break;
	case 0:
	    switch (longval) {
	    case 1:
		net_bw_limit_in = atoi(optarg) * 1024;
		break;
	    case 2:
		net_bw_limit_out = atoi(optarg) * 1024;
		break;
	    case 6:
		net_bw_hz = atoi(optarg);
		break;
	    default:
		usage();
	    }
	    break;
	case '?':
	default:
	    usage();
	}
    }
args_done:
    argc -= optind;
    argv += optind;

    if (argc != 0)
	usage();

    setup_daemon(dir);

    event_init();

    btpd_init();
    torrent_load("test");

    event_dispatch();
    btpd_err("Unexpected exit from libevent.\n");

    return 1;
}
