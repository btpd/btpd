AM_CFLAGS=-std=c99 -Wall @WARNNPS@
AM_CPPFLAGS=-I$(top_srcdir)/misc -I$(top_srcdir)/evloop @openssl_CPPFLAGS@ -D_GNU_SOURCE=1 -D_FILE_OFFSET_BITS=64 -D@EVLOOP_METHOD@
AM_LDFLAGS=@openssl_LDFLAGS@

bin_PROGRAMS=btpd/btpd cli/btcli cli/btinfo
noinst_LIBRARIES=misc/libmisc.a evloop/libevloop.a

EXTRA_DIST=COPYRIGHT CHANGES

# btpd
btpd_btpd_SOURCES=\
        btpd/active.c btpd/active.h btpd/addrinfo.c\
	btpd/btpd.c btpd/btpd.h\
	btpd/cli_if.c btpd/content.c btpd/content.h\
	btpd/download.c btpd/download_subr.c btpd/download.h\
	btpd/http_tr_if.c\
	btpd/main.c\
	btpd/nameconn.c btpd/net.c btpd/net.h btpd/net_types.h\
	btpd/net_buf.c btpd/net_buf.h\
	btpd/opts.c btpd/opts.h\
	btpd/peer.c btpd/peer.h\
	btpd/thread_cb.c btpd/tlib.c btpd/tlib.h btpd/torrent.c btpd/torrent.h\
	btpd/tracker_req.c btpd/tracker_req.h\
	btpd/upload.c btpd/upload.h\
	btpd/util.c
btpd_btpd_CFLAGS=-pthread $(AM_CFLAGS)
btpd_btpd_LDADD=misc/libmisc.a evloop/libevloop.a -lcrypto -lm @CLOCKLIB@

# btinfo
cli_btinfo_SOURCES=cli/btinfo.c
cli_btinfo_LDADD=misc/libmisc.a -lcrypto -lm

# btcli
cli_btcli_SOURCES=cli/btcli.c cli/btcli.h cli/add.c cli/del.c cli/list.c cli/kill.c cli/start.c cli/stop.c cli/stat.c
cli_btcli_LDADD=misc/libmisc.a -lcrypto -lm

# libmisc
misc_libmisc_a_SOURCES=\
	misc/benc.c misc/benc.h\
	misc/btpd_if.c misc/btpd_if.h misc/ipcdefs.h\
	misc/metainfo.c misc/metainfo.h\
	misc/hashtable.c misc/hashtable.h\
	misc/http_client.c misc/http_client.h\
	misc/iobuf.c misc/iobuf.h\
	misc/queue.h\
	misc/stream.c misc/stream.h\
	misc/subr.c misc/subr.h

# evloop
EXTRA_evloop_libevloop_a_SOURCES=evloop/epoll.c evloop/kqueue.c evloop/poll.c
evloop_libevloop_a_SOURCES=\
	evloop/evloop.h\
	evloop/timeheap.c evloop/timeheap.h evloop/timer.c
evloop_libevloop_a_LIBADD=@EVLOOP_IMPL@
evloop_libevloop_a_DEPENDENCIES=@EVLOOP_IMPL@
