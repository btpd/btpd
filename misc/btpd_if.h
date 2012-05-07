#ifndef BTPD_IPC_H
#define BTPD_IPC_H

enum ipc_err {
#define ERRDEF(name, msg) IPC_##name,
#include "ipcdefs.h"
#undef ERRDEF
    IPC_ERRCOUNT
};

enum ipc_type {
    IPC_TYPE_ERR,
    IPC_TYPE_BIN,
    IPC_TYPE_NUM,
    IPC_TYPE_STR
};

enum ipc_tval {
#define TVDEF(val, type, name) IPC_TVAL_##val,
#include "ipcdefs.h"
#undef TVDEF
    IPC_TVALCOUNT
};

enum ipc_dval {
    IPC_DVAL_MIN,
    IPC_DVAL_MAX
};

enum ipc_twc {
    IPC_TWC_ALL,
    IPC_TWC_ACTIVE,
    IPC_TWC_INACTIVE
};

enum ipc_tstate {
    IPC_TSTATE_INACTIVE,
    IPC_TSTATE_START,
    IPC_TSTATE_STOP,
    IPC_TSTATE_LEECH,
    IPC_TSTATE_SEED
};

#ifndef DAEMON

struct ipc;

struct ipc_get_res {
    enum ipc_type type;
    union {
        struct {
            const char *p;
            size_t l;
        } str;
        long long num;
    } v;
};

struct ipc_torrent {
    int by_hash;
    union {
        unsigned num;
        uint8_t hash[20];
    } u;
};

typedef void (*tget_cb_t)(int obji, enum ipc_err objerr,
    struct ipc_get_res *res, void *arg);

//typedef void (*dget_cb_t)(struct ipc_get_res *res, size_t nres, void *arg);

int ipc_open(const char *dir, struct ipc **out);
void ipc_close(struct ipc *ipc);

const char *ipc_strerror(enum ipc_err err);

enum ipc_err btpd_add(struct ipc *ipc, const char *mi, size_t mi_size,
    const char *content, const char *name, const char *label);
enum ipc_err btpd_del(struct ipc *ipc, struct ipc_torrent *tp);
enum ipc_err btpd_rate(struct ipc *ipc, unsigned up, unsigned down);
enum ipc_err btpd_start(struct ipc *ipc, struct ipc_torrent *tp);
enum ipc_err btpd_start_all(struct ipc *ipc);
enum ipc_err btpd_stop(struct ipc *ipc, struct ipc_torrent *tp);
enum ipc_err btpd_stop_all(struct ipc *ipc);
enum ipc_err btpd_die(struct ipc *ipc);
enum ipc_err btpd_get(struct ipc *ipc, enum ipc_dval *keys, size_t nkeys,
    tget_cb_t cb, void *arg);
enum ipc_err btpd_tget(struct ipc *ipc, struct ipc_torrent *tps, size_t ntps,
    enum ipc_tval *keys, size_t nkeys, tget_cb_t cb, void *arg);
enum ipc_err btpd_tget_wc(struct ipc *ipc, enum ipc_twc, enum ipc_tval *keys,
    size_t nkeys, tget_cb_t cb, void *arg);

#endif

#endif
