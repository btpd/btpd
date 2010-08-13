#include "btcli.h"
#include "utils.h"

void
usage_stat(void)
{
    printf(
        "Display stats for active torrents.\n"
        "\n"
        "Usage: stat [-i] [-w seconds] [torrent ...]\n"
        "\n"
        "Arguments:\n"
        "torrent ...\n"
        "\tOnly display stats for the given torrents.\n"
        "\n"
        "Options:\n"
        "-i\n"
        "\tDisplay individual lines for each torrent.\n"
        "\n"
        "-n\n"
        "\tDisplay the name of each torrent. Implies '-i'.\n"
        "\n"
        "-w n\n"
        "\tDisplay stats every n seconds.\n"
        "\n");
    exit(1);
}

struct btstat {
    unsigned num;
    enum ipc_tstate state;
    unsigned peers, tr_good;
    long long content_got, content_size, downloaded, uploaded, rate_up,
        rate_down, tot_up;
    uint32_t pieces_seen, torrent_pieces;
};

struct cbarg {
    int individual, names;
    struct btstat tot;
};

static enum ipc_tval stkeys[] = {
    IPC_TVAL_STATE,
    IPC_TVAL_NUM,
    IPC_TVAL_NAME,
    IPC_TVAL_PCOUNT,
    IPC_TVAL_TRGOOD,
    IPC_TVAL_PCCOUNT,
    IPC_TVAL_PCSEEN,
    IPC_TVAL_SESSUP,
    IPC_TVAL_SESSDWN,
    IPC_TVAL_TOTUP,
    IPC_TVAL_RATEUP,
    IPC_TVAL_RATEDWN,
    IPC_TVAL_CGOT,
    IPC_TVAL_CSIZE
};

#define NSTKEYS ARRAY_COUNT(stkeys)

static void
print_stat(struct btstat *st)
{
    print_percent(st->content_got, st->content_size);
    print_size(st->downloaded);
    print_rate(st->rate_down);
    print_size(st->uploaded);
    print_rate(st->rate_up);
    print_ratio(st->tot_up, st->content_size);
    printf("%4u ", st->peers);
    print_percent(st->pieces_seen, st->torrent_pieces);
    printf("%3u", st->tr_good);
    printf("\n");
}

static void
stat_cb(int obji, enum ipc_err objerr, struct ipc_get_res *res, void *arg)
{
    struct cbarg *cba = arg;
    struct btstat st, *tot = &cba->tot;
    if (objerr != IPC_OK || res[IPC_TVAL_STATE].v.num == IPC_TSTATE_INACTIVE)
        return;
    bzero(&st, sizeof(st));
    st.state = res[IPC_TVAL_STATE].v.num;
    st.num = res[IPC_TVAL_NUM].v.num;
    tot->torrent_pieces += (st.torrent_pieces = res[IPC_TVAL_PCCOUNT].v.num);
    tot->pieces_seen += (st.pieces_seen = res[IPC_TVAL_PCSEEN].v.num);
    tot->content_got += (st.content_got = res[IPC_TVAL_CGOT].v.num);
    tot->content_size += (st.content_size = res[IPC_TVAL_CSIZE].v.num);
    tot->downloaded += (st.downloaded = res[IPC_TVAL_SESSDWN].v.num);
    tot->uploaded += (st.uploaded = res[IPC_TVAL_SESSUP].v.num);
    tot->rate_down += (st.rate_down = res[IPC_TVAL_RATEDWN].v.num);
    tot->rate_up += (st.rate_up = res[IPC_TVAL_RATEUP].v.num);
    tot->peers += (st.peers = res[IPC_TVAL_PCOUNT].v.num);
    tot->tot_up += (st.tot_up = res[IPC_TVAL_TOTUP].v.num);
    tot->tr_good += (st.tr_good = res[IPC_TVAL_TRGOOD].v.num);
    if (cba->individual) {
        if (cba->names)
            printf("%.*s\n", (int)res[IPC_TVAL_NAME].v.str.l,
                res[IPC_TVAL_NAME].v.str.p);
        printf("%4u %c. ", st.num, tstate_char(st.state));
        print_stat(&st);
    }
}

static void
do_stat(int individual, int names, int seconds, struct ipc_torrent *tps,
    int ntps)
{
    enum ipc_err err;
    struct cbarg cba;
    int header = 1;
    if (names)
        individual = 1;
    cba.individual = individual;
    cba.names = names;
again:
    header--;
    if (header == 0) {
        if (individual) {
            header = 1;
            printf(" NUM ST ");
        } else
            header = 20;
        printf("  HAVE   DLOAD      RTDWN   ULOAD       RTUP   RATIO CONN"
            "  AVAIL  TR\n");
    }

    bzero(&cba.tot, sizeof(cba.tot));
    cba.tot.state = IPC_TSTATE_INACTIVE;
    if (tps == NULL)
        err = btpd_tget_wc(ipc, IPC_TWC_ACTIVE, stkeys, NSTKEYS,
            stat_cb, &cba);
    else
        err = btpd_tget(ipc, tps, ntps, stkeys, NSTKEYS, stat_cb, &cba);
    if (err != IPC_OK)
        diemsg("command failed (%s).\n", ipc_strerror(err));
    if (names)
        printf("-------\n");
    if (individual)
        printf("        ");
    print_stat(&cba.tot);
    if (seconds > 0) {
        sleep(seconds);
        goto again;
    }
}

static struct option stat_opts [] = {
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

void
cmd_stat(int argc, char **argv)
{
    int ch;
    int wflag = 0, iflag = 0, nflag = 0, seconds = 0;
    struct ipc_torrent *tps = NULL;
    int ntps = 0;
    char *endptr;
    while ((ch = getopt_long(argc, argv, "inw:", stat_opts, NULL)) != -1) {
        switch (ch) {
        case 'i':
            iflag = 1;
            break;
        case 'n':
            nflag = 1;
            break;
        case 'w':
            wflag = 1;
            seconds = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || seconds < 1)
                usage_stat();
            break;
        default:
            usage_stat();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc > 0) {
        tps = malloc(argc * sizeof(*tps));
        for (int i = 0; i < argc; i++) {
            if (torrent_spec(argv[i], &tps[ntps]))
                ntps++;
            else
                exit(1);

        }
    }
    btpd_connect();
    do_stat(iflag, nflag, seconds, tps, ntps);
}
