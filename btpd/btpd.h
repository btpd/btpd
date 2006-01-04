#ifndef BTPD_H
#define BTPD_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <assert.h>
#include <errno.h>
#include <event.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

#include "queue.h"

#include "benc.h"
#include "metainfo.h"
#include "iobuf.h"
#include "net_buf.h"
#include "net.h"
#include "peer.h"
#include "torrent.h"
#include "download.h"
#include "upload.h"
#include "subr.h"
#include "content.h"
#include "opts.h"

#define BTPD_VERSION (PACKAGE_NAME "/" PACKAGE_VERSION)

#define BTPD_L_ALL      0xffffffff
#define BTPD_L_ERROR    0x00000001
#define BTPD_L_TRACKER  0x00000002
#define BTPD_L_CONN     0x00000004
#define BTPD_L_MSG      0x00000008
#define BTPD_L_BTPD     0x00000010
#define BTPD_L_POL      0x00000020

void btpd_init(void);

void btpd_log(uint32_t type, const char *fmt, ...);

void btpd_err(const char *fmt, ...);

void *btpd_malloc(size_t size);
void *btpd_calloc(size_t nmemb, size_t size);

void btpd_shutdown(void);

void btpd_add_child(pid_t pid, void (*cb)(pid_t, void *), void *arg);

struct torrent * btpd_get_torrent(const uint8_t *hash);
const struct torrent_tq *btpd_get_torrents(void);
void btpd_add_torrent(struct torrent *tp);
void btpd_del_torrent(struct torrent *tp);
unsigned btpd_get_ntorrents(void);
const uint8_t *btpd_get_peer_id(void);

#endif
