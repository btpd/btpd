#include <sys/types.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "btpd.h"
#include "tracker_req.h"

extern void client_connection_cb(int sd, short type, void *arg);

struct btpd btpd;

void *
btpd_malloc(size_t size)
{
    void *a;
    if ((a = malloc(size)) == NULL)
	btpd_err("Failed to allocate %d bytes.\n", (int)size);
    return a;
}

void *
btpd_calloc(size_t nmemb, size_t size)
{
    void *a;
    if ((a = calloc(nmemb, size)) == NULL)
	btpd_err("Failed to allocate %d bytes.\n", (int)(nmemb * size));
    return a;
}

const char *
logtype_str(uint32_t type)
{
    if (type & BTPD_L_BTPD)
	return "btpd";
    else if (type & BTPD_L_ERROR)
	return "error";
    else if (type & BTPD_L_CONN)
	return "conn";
    else if (type & BTPD_L_TRACKER)
	return "tracker";
    else if (type & BTPD_L_MSG)
	return "msg";
    else
	return "";
}

void
btpd_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (BTPD_L_ERROR & btpd.logmask) {
	char tbuf[20];
	time_t tp = time(NULL);
	strftime(tbuf, 20, "%b %e %T", localtime(&tp));
	printf("%s %s: ", tbuf, logtype_str(BTPD_L_ERROR));
	vprintf(fmt, ap);
    }
    va_end(ap);
    exit(1);
}

void
btpd_log(uint32_t type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (type & btpd.logmask) {
	char tbuf[20];
	time_t tp = time(NULL);
	strftime(tbuf, 20, "%b %e %T", localtime(&tp));
	printf("%s %s: ", tbuf, logtype_str(type));
	vprintf(fmt, ap);
    }
    va_end(ap);
}

static void
btpd_init(void)
{
    bcopy(BTPD_VERSION, btpd.peer_id, sizeof(BTPD_VERSION) - 1);
    btpd.peer_id[sizeof(BTPD_VERSION) - 1] = '|';
    srandom(time(NULL));
    for (int i = sizeof(BTPD_VERSION); i < 20; i++)
	btpd.peer_id[i] = rint(random() * 255.0 / RAND_MAX);

    btpd.version = BTPD_VERSION;

    btpd.logmask = BTPD_L_BTPD | BTPD_L_ERROR;

    BTPDQ_INIT(&btpd.kids);

    btpd.ntorrents = 0;
    BTPDQ_INIT(&btpd.cm_list);

    BTPDQ_INIT(&btpd.readq);
    BTPDQ_INIT(&btpd.writeq);

    BTPDQ_INIT(&btpd.unattached);

    btpd.port = 6881;

    btpd.bw_hz = 8;
    btpd.bwcalls = 0;
    for (int i = 0; i < BWCALLHISTORY; i++)
	btpd.bwrate[i] = 0;

    btpd.obwlim = 0;
    btpd.ibwlim = 0;
    btpd.obw_left = 0;
    btpd.ibw_left = 0;

    btpd.npeers = 0;

    int nfiles = getdtablesize();
    if (nfiles <= 20)
	btpd_err("Too few open files allowed (%d). "
		 "Check \"ulimit -n\"\n", nfiles);
    else if (nfiles < 64)
	btpd_log(BTPD_L_BTPD,
		 "You have restricted the number of open files to %d. "
		 "More could be beneficial to the download performance.\n",
		 nfiles);
    btpd.maxpeers = nfiles - 20;
}

void
btpd_shutdown(void)
{
    struct torrent *tp;

    tp = BTPDQ_FIRST(&btpd.cm_list);
    while (tp != NULL) {
        struct torrent *next = BTPDQ_NEXT(tp, entry);
        torrent_unload(tp);
        tp = next;
    }
    btpd_log(BTPD_L_BTPD, "Exiting.\n");
    exit(0);
}

static void
signal_cb(int signal, short type, void *arg)
{
    btpd_log(BTPD_L_BTPD, "Got signal %d.\n", signal);
    btpd_shutdown();
}

static void
child_cb(int signal, short type, void *arg)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
	if (WIFEXITED(status) || WIFSIGNALED(status)) {
	    struct child *kid = BTPDQ_FIRST(&btpd.kids);
	    while (kid != NULL && kid->pid != pid)
		kid = BTPDQ_NEXT(kid, entry);
	    assert(kid != NULL);
	    BTPDQ_REMOVE(&btpd.kids, kid, entry);
	    kid->child_done(kid);
	}
    }
}

static void
heartbeat_cb(int sd, short type, void *arg)
{
    struct torrent *tp;

    btpd.seconds++;

    net_bw_rate();

    BTPDQ_FOREACH(tp, &btpd.cm_list, entry)
	cm_by_second(tp);

    evtimer_add(&btpd.heartbeat, (& (struct timeval) { 1, 0 }));
}

static void
usage()
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
    { "logfile", required_argument,	&longval,	3 },
    { "ipc", 	required_argument,	&longval,	4 },
    { "help",	no_argument,		&longval,	5 },
    { NULL,	0,			NULL,		0 }
};

int
main(int argc, char **argv)
{
    int error, ch;
    char *logfile = NULL, *ipc = NULL;
    int d_opt = 0;

    setlocale(LC_ALL, "");
    btpd_init();

    while ((ch = getopt_long(argc, argv, "dp:", longopts, NULL)) != -1) {
	switch (ch) {
	case 'd':
	    d_opt = 1;
	    break;
	case 'p':
	    btpd.port = atoi(optarg);
	    break;
	case 0:
	    switch (longval) {
	    case 1:
		btpd.ibwlim = atoi(optarg) * 1024;
		break;
	    case 2:
		btpd.obwlim = atoi(optarg) * 1024;
		break;
	    case 3:
		logfile = optarg;
		break;
	    case 4:
		ipc = optarg;
		for (int i = 0; i < strlen(ipc); i++)
		    if (!isalnum(ipc[i]))
			btpd_err("--ipc only takes letters and digits.\n");
		break;
	    case 5:
		usage();
	    case 6:
		btpd.bw_hz = atoi(optarg);
		if (btpd.bw_hz <= 0 || btpd.bw_hz > 100)
		    btpd_err("I will only accept bw limiter hz "
			"between 1 and 100.\n");
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
    argc -= optind;
    argv += optind;

    if (argc != 0)
	usage();

    //net_init();
    {
	int sd;
	int flag = 1;
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(btpd.port);

	if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	    btpd_err("socket: %s\n", strerror(errno));
	setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
	    btpd_err("bind: %s\n", strerror(errno));
	listen(sd, 10);
	set_nonblocking(sd);
	btpd.peer4_sd = sd;
    }

    //ipc_init();
    {
	int sd;
	struct sockaddr_un addr;
	size_t psiz = sizeof(addr.sun_path);

	addr.sun_family = PF_UNIX;
	if (ipc != NULL) {
	    if (snprintf(addr.sun_path, psiz, "/tmp/btpd_%u_%s",
			 geteuid(), ipc) >= psiz)
		btpd_err("%s is too long.\n", ipc);
	} else
	    snprintf(addr.sun_path, psiz, "/tmp/btpd_%u_default", geteuid());

	if ((sd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
	    btpd_err("sock: %s\n", strerror(errno));
	if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
	    if (errno == EADDRINUSE) {
		if (connect(sd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		    btpd_err("btpd already running at %s.\n", addr.sun_path);
		else {
		    unlink(addr.sun_path);
		    if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
			btpd_err("bind: %s\n", strerror(errno));
		}
	    } else
		btpd_err("bind: %s\n", strerror(errno));
	}
	if (chmod(addr.sun_path, 0600) == -1)
	    btpd_err("chmod: %s (%s).\n", addr.sun_path, strerror(errno));
	listen(sd, 4);
	set_nonblocking(sd);
	btpd.ipc_sd = sd;
    }

    freopen("/dev/null", "r", stdin);
    if (logfile == NULL)
	logfile = "btpd.log";
    if (!d_opt) {
	freopen(logfile, "w", stdout);
	freopen(logfile, "w", stderr);
	daemon(1, 1);
    }

    setlinebuf(stdout);
    setlinebuf(stderr);

    event_init();

    signal(SIGPIPE, SIG_IGN);

    signal_set(&btpd.sigint, SIGINT, signal_cb, NULL);
    signal_add(&btpd.sigint, NULL);
    signal_set(&btpd.sigterm, SIGTERM, signal_cb, NULL);
    signal_add(&btpd.sigterm, NULL);
    signal_set(&btpd.sigchld, SIGCHLD, child_cb, NULL);
    signal_add(&btpd.sigchld, NULL);

    evtimer_set(&btpd.heartbeat, heartbeat_cb,  NULL);
    evtimer_add(&btpd.heartbeat, (& (struct timeval) { 1, 0 }));

    event_set(&btpd.cli, btpd.ipc_sd, EV_READ | EV_PERSIST,
        client_connection_cb, &btpd);
    event_add(&btpd.cli, NULL);

    event_set(&btpd.accept4, btpd.peer4_sd, EV_READ | EV_PERSIST,
        net_connection_cb, &btpd);
    event_add(&btpd.accept4, NULL);

    evtimer_set(&btpd.bwlim, net_bw_cb, NULL);
    if (btpd.obwlim > 0 || btpd.ibwlim > 0) {
	btpd.ibw_left = btpd.ibwlim / btpd.bw_hz;
	btpd.obw_left = btpd.obwlim / btpd.bw_hz;
	evtimer_add(&btpd.bwlim,
	    (& (struct timeval) { 0, 1000000 / btpd.bw_hz }));
    }

    error = event_dispatch();
    btpd_err("Returned from dispatch. Error = %d.\n", error);

    return error;
}
