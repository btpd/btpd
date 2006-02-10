bin_PROGRAMS=btpd
btpd_SOURCES=\
	btpd.c btpd.h\
	cli_if.c content.c content.h\
	download.c download_subr.c download.h\
	http.c http.h\
	main.c\
	net.c net.h net_types.h\
	net_buf.c net_buf.h\
	opts.c opts.h\
	peer.c peer.h\
	queue.h\
	torrent.c torrent.h\
	tracker_req.c tracker_req.h\
	upload.c upload.h\
	util.c

CC=@PTHREAD_CC@
CFLAGS=@CFLAGS@ @PTHREAD_CFLAGS@ @curl_CFLAGS@

btpd_LDADD=@PTHREAD_LIBS@ ../misc/libmisc.a -levent -lcrypto -lm
btpd_CPPFLAGS=-I$(top_srcdir)/misc @event_CPPFLAGS@ @openssl_CPPFLAGS@
btpd_LDFLAGS=@event_LDFLAGS@ @openssl_LDFLAGS@ @curl_LDFLAGS@
