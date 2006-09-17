#include "btcli.h"

void
usage_list(void)
{
    printf(
        "List torrents.\n"
        "\n"
        "Usage: list [-a] [-i]\n"
        "\n"
        );
    exit(1);
}

struct item {
    unsigned num;
    char *name;
    char st;
    BTPDQ_ENTRY(item) entry;
};

struct items {
    int count;
    BTPDQ_HEAD(item_tq, item) hd;
};

void
itm_insert(struct items *itms, struct item *itm)
{
    struct item *p;
    BTPDQ_FOREACH(p, &itms->hd, entry)
        if (itm->num < p->num)
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
    itms->count++;
    itm->num = (unsigned)res[IPC_TVAL_NUM].v.num;
    itm->st = tstate_char(res[IPC_TVAL_STATE].v.num);
    if (res[IPC_TVAL_NAME].type == IPC_TYPE_ERR)
        asprintf(&itm->name, "%s", ipc_strerror(res[IPC_TVAL_NAME].v.num));
    else
        asprintf(&itm->name, "%.*s", (int)res[IPC_TVAL_NAME].v.str.l,
            res[IPC_TVAL_NAME].v.str.p);
    itm_insert(itms, itm);
}

void
print_items(struct items* itms)
{
    int n;
    struct item *p;
    BTPDQ_FOREACH(p, &itms->hd, entry) {
        n = printf("%u: ", p->num);
        while (n < 7) {
            putchar(' ');
            n++;
        }
        printf("%c. %s\n", p->st, p->name);
    }
}

static struct option list_opts [] = {
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

void
cmd_list(int argc, char **argv)
{
    int ch, inactive = 0, active = 0;
    enum ipc_err code;
    enum ipc_twc twc;
    enum ipc_tval keys[] = { IPC_TVAL_NUM, IPC_TVAL_STATE, IPC_TVAL_NAME };
    struct items itms;
    while ((ch = getopt_long(argc, argv, "ai", list_opts, NULL)) != -1) {
        switch (ch) {
        case 'a':
            active = 1;
            break;
        case 'i':
            inactive = 1;
            break;
        default:
            usage_list();
        }
    }

    if (inactive == active)
        twc = IPC_TWC_ALL;
    else if (inactive)
        twc = IPC_TWC_INACTIVE;
    else
        twc = IPC_TWC_ACTIVE;

    btpd_connect();
    printf("NUM    ST NAME\n");
    itms.count = 0;
    BTPDQ_INIT(&itms.hd);
    if ((code = btpd_tget_wc(ipc, twc, keys, 3, list_cb, &itms)) != IPC_OK)
        errx(1, "%s", ipc_strerror(code));
    print_items(&itms);
    printf("Listed %d torrent%s.\n", itms.count, itms.count == 1 ? "" : "s");
}
