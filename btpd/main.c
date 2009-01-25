#include "btpd.h"

#include <getopt.h>
#include <time.h>

int btpd_daemon_phase = 2;
int first_btpd_comm[2];
int pidfd;

void
first_btpd_exit(char code)
{
    write(first_btpd_comm[1], &code, 1);
    close(first_btpd_comm[0]);
    close(first_btpd_comm[1]);
}

static void
writepid(void)
{
    int nw;
    char pidtxt[100];
    nw = snprintf(pidtxt, sizeof(pidtxt), "%ld", (long)getpid());
    ftruncate(pidfd, 0);
    write(pidfd, pidtxt, nw);
}

static void
setup_daemon(int daemonize, const char *dir)
{
    char c;
    pid_t pid;
    struct timespec ts;

    if (snprintf(NULL, 0, "btpd") != 4)
        btpd_err("snprintf doesn't work.\n");

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        btpd_err("clock_gettime(CLOCK_MONOTONIC, ...) failed (%s).\n",
            strerror(errno));

    if (dir == NULL) {
        if ((dir = find_btpd_dir()) == NULL)
            btpd_err("Cannot find the btpd directory.\n");
        if (dir[0] != '/')
            btpd_err("Got non absolute path '%s' from system environment.\n",
                dir);
        btpd_dir = dir;
    }

    if (mkdir(dir, 0777) == -1 && errno != EEXIST)
        btpd_err("Couldn't create home '%s' (%s).\n", dir, strerror(errno));

    if (chdir(dir) != 0)
        btpd_err("Couldn't change working directory to '%s' (%s).\n", dir,
            strerror(errno));

    if (mkdir("torrents", 0777) == -1 && errno != EEXIST)
        btpd_err("Couldn't create torrents subdir (%s).\n", strerror(errno));

    if (btpd_dir == NULL) {
        char wd[PATH_MAX];
        if (getcwd(wd, PATH_MAX) == NULL)
            btpd_err("Couldn't get working directory (%s).\n",
                strerror(errno));
        if ((btpd_dir = strdup(wd)) == NULL)
            btpd_err("Out of memory.\n");
    }

    if (daemonize) {
        if (pipe(first_btpd_comm) < 0)
            btpd_err("Failed to create pipe (%s).\n", strerror(errno));
        if ((pid = fork()) < 0)
            btpd_err("fork() failed (%s).\n", strerror(errno));
        if (pid != 0) {
            read(first_btpd_comm[0], &c, 1);
            exit(c);
        }
        btpd_daemon_phase--;
        if (setsid() < 0)
            btpd_err("setsid() failed (%s).\n", strerror(errno));
        if ((pid = fork()) < 0)
            btpd_err("fork() failed (%s).\n", strerror(errno));
        if (pid != 0)
            exit(0);
    }

    if ((pidfd = open("pid", O_CREAT|O_WRONLY, 0666)) == -1)
        btpd_err("Couldn't open 'pid' (%s).\n", strerror(errno));

    if (lockf(pidfd, F_TLOCK, 0) == -1)
        btpd_err("Another instance of btpd is probably running in %s.\n", dir);

    writepid();
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
        "-4\n"
        "\tToggle use of IPv4. It's enabled by default.\n"
        "\n"
        "-6\n"
        "\tToggle use of IPv6. It's enabled by default.\n"
        "\n"
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
        "\tLet the tracker distribute the given address instead of the one\n"
        "\tit sees btpd connect from.\n"
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
        switch (getopt_long(argc, argv, "46d:p:", longopts, NULL)) {
        case -1:
            goto args_done;
        case '6':
            net_ipv6 ^= 1;
            break;
        case '4':
            net_ipv4 ^= 1;
            break;
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
                tr_ip_arg = optarg;
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

    if (!net_ipv4 && !net_ipv6)
        btpd_err("You need to enable at least one ip version.\n");

    if (argc > 0)
        usage();

    setup_daemon(daemonize, dir);

    if (evloop_init() != 0)
        btpd_err("Failed to initialize evloop (%s).\n", strerror(errno));

    btpd_init();

    if (daemonize) {
        if (freopen("/dev/null", "r", stdin) == NULL)
            btpd_err("freopen of stdin failed (%s).\n", strerror(errno));
        if (freopen(log == NULL ? "log" : log, "a", stderr) == NULL)
            btpd_err("Couldn't open '%s' (%s).\n", log, strerror(errno));
        if (dup2(fileno(stderr), fileno(stdout)) < 0)
            btpd_err("dup2 failed (%s).\n", strerror(errno));
        first_btpd_exit(0);
    }
    setlinebuf(stdout);
    setlinebuf(stderr);

    btpd_daemon_phase = 0;

    if (!empty_start)
        active_start();
    else
        active_clear();

    evloop();

    btpd_err("Exit from evloop with error (%s).\n", strerror(errno));

    return 1;
}
