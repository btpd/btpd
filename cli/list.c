#include "btcli.h"

void
usage_list(void)
{
    printf(
        "List torrents.\n"
        "\n"
        "Usage: list [-a] [-i] [-f <format>]\n"
        "       list torrent ...\n"
        "\n"
        "Arguments:\n"
        "torrent ...\n"
        "\tThe torrents to list. Running 'btcli list' without any arguments\n"
        "\tor options is equivalent to running 'btcli list -ai'.\n"
        "\n"
        "Options:\n"
        "-a\n"
        "\tList active torrents.\n"
        "\n"
        "-i\n"
        "\tList inactive torrents.\n"
        "\n"
        );
    exit(1);
}

struct item {
    unsigned num, peers;
    char *name, *dir;
    char hash[SHAHEXSIZE];
    char st;
    long long cgot, csize, totup, downloaded, uploaded, rate_up, rate_down;
    uint32_t torrent_pieces, pieces_have, pieces_seen;
    BTPDQ_ENTRY(item) entry;
};

struct items {
    int count;
    char **argv;
    int ntps;
    struct ipc_torrent *tps;
    BTPDQ_HEAD(item_tq, item) hd;
};

void
itm_insert(struct items *itms, struct item *itm)
{
    struct item *p;
    BTPDQ_FOREACH(p, &itms->hd, entry)
        if (strcmp(itm->name, p->name) < 0)
            break;
    if (p != NULL)
        BTPDQ_INSERT_BEFORE(p, itm, entry);
    else
        BTPDQ_INSERT_TAIL(&itms->hd, itm, entry);
}

static void
list_cb(int obji, enum ipc_err objerr, struct ipc_get_res *res, void *arg)
{
    struct items *itms = arg;
    struct item *itm = calloc(1, sizeof(*itm));
    if (objerr != IPC_OK)
        diemsg("list failed for '%s' (%s).\n", itms->argv[obji],
            ipc_strerror(objerr));
    itms->count++;
    itm->num   = (unsigned)res[IPC_TVAL_NUM].v.num;
    itm->peers = (unsigned)res[IPC_TVAL_PCOUNT].v.num;
    itm->st = tstate_char(res[IPC_TVAL_STATE].v.num);
    if (res[IPC_TVAL_NAME].type == IPC_TYPE_ERR)
        asprintf(&itm->name, "%s", ipc_strerror(res[IPC_TVAL_NAME].v.num));
    else
        asprintf(&itm->name, "%.*s", (int)res[IPC_TVAL_NAME].v.str.l,
            res[IPC_TVAL_NAME].v.str.p);
    if (res[IPC_TVAL_DIR].type == IPC_TYPE_ERR)
        asprintf(&itm->dir, "%s", ipc_strerror(res[IPC_TVAL_DIR].v.num));
    else
        asprintf(&itm->dir, "%.*s", (int)res[IPC_TVAL_DIR].v.str.l,
            res[IPC_TVAL_DIR].v.str.p);
    bin2hex(res[IPC_TVAL_IHASH].v.str.p, itm->hash, 20);
    itm->cgot           = res[IPC_TVAL_CGOT].v.num;
    itm->csize          = res[IPC_TVAL_CSIZE].v.num;
    itm->totup          = res[IPC_TVAL_TOTUP].v.num;
    itm->downloaded     = res[IPC_TVAL_SESSDWN].v.num;
    itm->uploaded       = res[IPC_TVAL_SESSUP].v.num;
    itm->rate_up        = res[IPC_TVAL_RATEUP].v.num;
    itm->rate_down      = res[IPC_TVAL_RATEDWN].v.num;
    itm->torrent_pieces = (uint32_t)res[IPC_TVAL_PCCOUNT].v.num;
    itm->pieces_seen    = (uint32_t)res[IPC_TVAL_PCSEEN].v.num;
    itm->pieces_have    = (uint32_t)res[IPC_TVAL_PCGOT].v.num;

    itm_insert(itms, itm);
}

void
print_items(struct items* itms, char *format)
{
    struct item *p;
    char *it;
    BTPDQ_FOREACH(p, &itms->hd, entry) {
        if(format) {
            for (it = format; *it; ++it) {
                switch (*it) {
                    case '%':
                        ++it;
                        switch (*it) {
                            case '%': putchar('%');                      break;
                            case '#': printf("%u",   p->num);            break;
                            case '^': printf("%lld", p->rate_up);        break;

                            case 'A': printf("%u",   p->pieces_seen);    break;
                            case 'D': printf("%lld", p->downloaded);     break;
                            case 'H': printf("%u",   p->pieces_have);    break;
                            case 'P': printf("%u",   p->peers);          break;
                            case 'S': printf("%c",   p->st);             break;
                            case 'U': printf("%lld", p->uploaded);       break;
                            case 'T': printf("%u",   p->torrent_pieces); break;

                            case 'd': printf("%s",   p->dir);            break;
                            case 'g': printf("%lld", p->cgot);           break;
                            case 'h': printf("%s",   p->hash);           break;
                            case 'n': printf("%s",   p->name);           break;
                            case 'p': print_percent(p->cgot, p->csize);  break;
                            case 'r': print_ratio(p->totup, p->csize);   break;
                            case 's': print_size(p->csize);              break;
                            case 't': printf("%lld", p->csize);          break;
                            case 'u': printf("%lld", p->totup);          break;
                            case 'v': printf("%lld", p->rate_down);      break;

                            case '\0': continue;
                        }
                        break;
                    case '\\':
                        ++it;
                        switch (*it) {
                            case 'n':  putchar('\n'); break;
                            case 't':  putchar('\t'); break;
                            case '\0': continue;
                        }
                        break;
                    default: putchar(*it); break;
                }
            }
        } else {
            printf("%-40.40s %4u %c. ", p->name, p->num, p->st);
            print_percent(p->cgot, p->csize);
            print_size(p->csize);
            print_ratio(p->totup, p->csize);
            printf("\n");
        }
    }
}

static struct option list_opts [] = {
    { "format", required_argument, NULL, 'f' },
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

void
cmd_list(int argc, char **argv)
{
    int ch, inactive = 0, active = 0;
    char *format = NULL;
    enum ipc_err code;
    enum ipc_twc twc;
    enum ipc_tval keys[] = { IPC_TVAL_NUM,    IPC_TVAL_STATE,   IPC_TVAL_NAME,
           IPC_TVAL_TOTUP,   IPC_TVAL_CSIZE,  IPC_TVAL_CGOT,    IPC_TVAL_PCOUNT,
           IPC_TVAL_PCCOUNT, IPC_TVAL_PCSEEN, IPC_TVAL_PCGOT,   IPC_TVAL_SESSUP,
           IPC_TVAL_SESSDWN, IPC_TVAL_RATEUP, IPC_TVAL_RATEDWN, IPC_TVAL_IHASH,
           IPC_TVAL_DIR };
    size_t nkeys = sizeof(keys) / sizeof(keys[0]);
    struct items itms;
    while ((ch = getopt_long(argc, argv, "aif:", list_opts, NULL)) != -1) {
        switch (ch) {
        case 'a':
            active = 1;
            break;
        case 'f':
            format = optarg;
            break;
        case 'i':
            inactive = 1;
            break;
        default:
            usage_list();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc > 0) {
        if (inactive || active)
            usage_list();
        itms.tps = malloc(argc * sizeof(*itms.tps));
        for (itms.ntps = 0; itms.ntps < argc; itms.ntps++) {
            if (!torrent_spec(argv[itms.ntps], &itms.tps[itms.ntps]))
                exit(1);

        }
    } else {
        itms.ntps = 0;
        itms.tps = NULL;
    }
    if (inactive == active)
        twc = IPC_TWC_ALL;
    else if (inactive)
        twc = IPC_TWC_INACTIVE;
    else
        twc = IPC_TWC_ACTIVE;

    btpd_connect();
    itms.count = 0;
    itms.argv = argv;
    BTPDQ_INIT(&itms.hd);
    if (itms.tps == NULL)
        code = btpd_tget_wc(ipc, twc, keys, nkeys, list_cb, &itms);
    else
        code = btpd_tget(ipc, itms.tps, itms.ntps, keys, nkeys, list_cb, &itms);
    if (code != IPC_OK)
        diemsg("command failed (%s).\n", ipc_strerror(code));
    if (format == NULL)
        printf("%-40.40s  NUM ST   HAVE    SIZE   RATIO\n", "NAME");
    print_items(&itms, format);
}
