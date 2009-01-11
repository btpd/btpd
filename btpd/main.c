#include "btpd.h"

#include <sys/file.h>
#include <err.h>
#include <getopt.h>
#include <time.h>

static void
writepid(int pidfd)
{
    FILE *fp = fdopen(dup(pidfd), "w");
    fprintf(fp, "%ld", (long)getpid());
    fclose(fp);
}

static void
setup_daemon(int daemonize, const char *dir, const char *log)
{
    int pidfd;
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        errx(1, "clock_gettime(CLOCK_MONOTONIC, ...) error (%s).",
            strerror(errno));

    if (log == NULL)
        log = "log";

    if (dir == NULL) {
        if ((dir = find_btpd_dir()) == NULL)
            errx(1, "Cannot find the btpd directory");
        else if (dir[0] != '/')
            errx(1, "got non absolute path '%s' from system environment.",
                dir);
        btpd_dir = dir;
    }

    if (mkdir(dir, 0777) == -1 && errno != EEXIST)
        err(1, "Couldn't create home '%s'", dir);

    if (chdir(dir) != 0)
        err(1, "Couldn't change working directory to '%s'", dir);

    if (mkdir("torrents", 0777) == -1 && errno != EEXIST)
        err(1, "Couldn't create torrents subdir");

    if ((pidfd = open("pid", O_CREAT|O_TRUNC|O_WRONLY, 0666)) == -1)
        err(1, "Couldn't open 'pid'");

    if (flock(pidfd, LOCK_NB|LOCK_EX) == -1)
        errx(1, "Another instance of btpd is probably running in %s.", dir);

    if (btpd_dir == NULL) {
        char wd[PATH_MAX];
        if (getcwd(wd, PATH_MAX) == NULL)
            err(1, "couldn't get working directory");
        btpd_dir = strdup(wd);
    }

    if (daemonize) {
        if (daemon(1, 1) != 0)
            err(1, "Failed to daemonize");
        freopen("/dev/null", "r", stdin);
        if (freopen(log, "a", stdout) == NULL)
            err(1, "Couldn't open '%s'", log);
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
        "btpd is the BitTorrent Protocol Daemon.\n"
        "\n"
        "Usage: btpd [-d dir] [-p port] [more options...]\n"
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
        "-d dir\n"
        "\tThe directory in which to run btpd. Default is '$HOME/.btpd'.\n"
        "\n"
        "--empty-start\n"
        "\tStart btpd without any active torrents.\n"
        "\n"
        "--help\n"
        "\tShow this text.\n"
        "\n"
        "--ip addr\n"
        "\tMake other peers use the given address, instead of the one\n"
        "\tthe tracker perceives as this peer's address, when contacting\n"
        "\tthis peer.\n"
        "\n"
        "--ipcprot mode\n"
        "\tSet the protection mode of the command socket.\n"
        "\tThe mode is specified by an octal number. Default is 0600.\n"
        "\n"
        "--logfile file\n"
        "\tWhere to put the logfile. By default it's put in the btpd dir.\n"
        "\n"
        "--max-peers n\n"
        "\tLimit the amount of peers to n.\n"
        "\n"
        "--max-uploads n\n"
        "\tControls the number of simultaneous uploads.\n"
        "\tThe possible values are:\n"
        "\t\tn < -1 : Choose n >= 2 based on --bw-out (default).\n"
        "\t\tn = -1 : Upload to every interested peer.\n"
        "\t\tn =  0 : Dont't upload to anyone.\n"
        "\t\tn >  0 : Upload to at most n peers simultaneously.\n"
        "\n"
        "--no-daemon\n"
        "\tKeep the btpd process in the foregorund and log to std{out,err}.\n"
        "\tThis option is intended for debugging purposes.\n"
        "\n"
        "-p n, --port n\n"
        "\tListen at port n. Default is 6881.\n"
        "\n"
        "--prealloc n\n"
        "\tPreallocate disk space in chunks of n kB. Default is 2048.\n"
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
    { "max-uploads", required_argument, &longval,       4 },
    { "max-peers", required_argument,   &longval,       5 },
    { "no-daemon", no_argument,         &longval,       6 },
    { "logfile", required_argument,     &longval,       7 },
    { "ipcprot", required_argument,     &longval,       8 },
    { "empty-start", no_argument,       &longval,       9 },
    { "ip", required_argument,          &longval,       10 },
    { "help",   no_argument,            &longval,       128 },
    { NULL,     0,                      NULL,           0 }
};

int
main(int argc, char **argv)
{
    char *dir = NULL, *log = NULL;
    int daemonize = 1;

    for (;;) {
        switch (getopt_long(argc, argv, "d:p:", longopts, NULL)) {
        case -1:
            goto args_done;
        case 'd':
            dir = optarg;
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
                net_max_uploads = atoi(optarg);
                break;
            case 5:
                net_max_peers = atoi(optarg);
                break;
            case 6:
                daemonize = 0;
                break;
            case 7:
                log = optarg;
                break;
            case 8:
                ipcprot = strtol(optarg, NULL, 8);
                break;
            case 9:
                empty_start = 1;
                break;
            case 10:
                switch (inet_pton(AF_INET, optarg, &tr_ip_arg)) {
                case 1:
                    break;
                case 0:
                    errx(1, "You must specify a dotted IPv4 address.\n");
                    break;
                default:
                    err(1, "inet_ntop %s", optarg);
                }
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

    if (argc > 0)
        usage();

    setup_daemon(daemonize, dir, log);

    if (evloop_init() != 0)
        btpd_err("Failed to initialize evloop (%s).\n", strerror(errno));

    btpd_init();

    evloop();

    btpd_err("Exit from evloop with error (%s).\n", strerror(errno));

    return 1;
}
