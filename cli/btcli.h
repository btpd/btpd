#ifndef BTCLI_H
#define BTCLI_H

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "btpd_if.h"
#include "metainfo.h"
#include "subr.h"
#include "benc.h"
#include "iobuf.h"
#include "queue.h"

extern const char *btpd_dir;
extern struct ipc *ipc;

void btpd_connect(void);
enum ipc_err handle_ipc_res(enum ipc_err err, const char *cmd,
    const char *target);
char tstate_char(enum ipc_tstate ts);
int torrent_spec(char *arg, struct ipc_torrent *tp);

void print_rate(long long rate);
void print_size(long long size);
void print_ratio(long long part, long long whole);
void print_percent(long long part, long long whole);

void usage_add(void);
void cmd_add(int argc, char **argv);
void usage_del(void);
void cmd_del(int argc, char **argv);
void usage_list(void);
void cmd_list(int argc, char **argv);
void usage_stat(void);
void cmd_stat(int argc, char **argv);
void usage_kill(void);
void cmd_kill(int argc, char **argv);
void usage_start(void);
void cmd_start(int argc, char **argv);
void usage_stop(void);
void cmd_stop(int argc, char **argv);

#endif
