bin_PROGRAMS=btpd
btpd_SOURCES=\
	main.c util.c\
	btpd.c btpd.h\
	opts.c opts.h\
	net.c net.h\
	net_buf.c net_buf.h\
	queue.h \
	peer.c peer.h\
	content.c content.h\
	download.c download_subr.c download.h\
	torrent.c torrent.h\
	tracker_req.c tracker_req.h\
	upload.c upload.h

btpd_LDADD=../misc/libmisc.a -levent -lcrypto -lm
btpd_CPPFLAGS=-I$(top_srcdir)/misc @event_CPPFLAGS@ @openssl_CPPFLAGS@
btpd_CFLAGS=@CURL_CFLAGS@
btpd_LDFLAGS=@event_LDFLAGS@ @openssl_LDFLAGS@ @CURL_LDFLAGS@
