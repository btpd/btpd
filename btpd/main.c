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

    if (mkdir("library", 0777) == -1 && errno != EEXIST)
        err(1, "Couldn't create library");

    pidfd = open("pid", O_CREAT|O_WRONLY|O_NONBLOCK|O_EXLOCK, 0666);
    if (pidfd == -1) {
        if (errno == EAGAIN)
            errx(1, "Another instance of btpd is probably running in %s.",
                dir);
        else
            err(1, "Couldn't open 'pid'");
    }

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
    printf(
        "The BitTorrent Protocol Daemon.\n"
        "\n"
        "Usage: btpd [options] [dir]\n"
        "\n"
        "Arguments:\n"
        "dir\n"
        "\tThe directory in which to run btpd. Default is '$HOME/.btpd'.\n"
        "\n"
        "Options:\n"
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
        "--downloaders n\n"
        "\tControls the number of simultaneous uploads.\n"
        "\tThe possible values are:\n"
        "\t\tn < -1 : Choose n >= 2 based on --bw-out (default).\n"
        "\t\tn = -1 : Upload to every interested peer.\n"
        "\t\tn =  0 : Dont't upload to anyone.\n"
        "\t\tn >  0 : Upload to at most n peers simultaneously.\n"
        "\n"
        "--help\n"
        "\tShow this text.\n"
        "\n"
        "--max-peers n\n"
        "\tLimit the amount of peers to n.\n"
        "\n"
        "-p n, --port n\n"
        "\tListen at port n. Default is 6881.\n"
        "\n"
        "--prealloc n\n"
        "\tPreallocate disk space in chunks of n kB. Default is 1.\n"
        "\tNote that n will be rounded up to the closest multiple of the\n"
        "\ttorrent piece size. If n is zero no preallocation will be done.\n"
        "\n");
    exit(1);
}

static int longval = 0;

static struct option longopts[] = {
    { "port",   required_argument,      NULL,           'p' },
    { "bw-in",  required_argument,      &longval,       1 },
    { "bw-out", required_argument,      &longval,       2 },
    { "prealloc", required_argument,    &longval,       3 },
    { "downloaders", required_argument, &longval,       4 },
    { "max-peers", required_argument,   &longval,       5 },
    { "help",   no_argument,            &longval,       128 },
    { NULL,     0,                      NULL,           0 }
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
            case 3:
                cm_alloc_size = atoi(optarg) * 1024;
                break;
            case 4:
                net_max_downloaders = atoi(optarg);
                break;
            case 5:
                net_max_peers = atoi(optarg);
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

    if (argc > 1)
        usage();
    if (argc > 0)
        dir = argv[0];

    setup_daemon(dir);

    event_init();

    btpd_init();

    event_dispatch();

    btpd_err("Unexpected exit from libevent.\n");

    return 1;
}
