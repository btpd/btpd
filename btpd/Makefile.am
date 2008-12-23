bin_PROGRAMS=btpd
btpd_SOURCES=\
        active.c active.h addrinfo.c\
	btpd.c btpd.h\
	cli_if.c content.c content.h\
	download.c download_subr.c download.h\
	http_tr_if.c\
	main.c\
	net.c net.h net_types.h\
	net_buf.c net_buf.h\
	opts.c opts.h\
	peer.c peer.h\
	thread_cb.c tlib.c tlib.h torrent.c torrent.h\
	tracker_req.c tracker_req.h\
	upload.c upload.h\
	util.c

btpd_LDADD=../misc/libmisc.a ../libevent/libevent.a -lcrypto -lm
btpd_CPPFLAGS=-I$(top_srcdir)/misc -I$(top_srcdir)/libevent @openssl_CPPFLAGS@
btpd_LDFLAGS=@openssl_LDFLAGS@
