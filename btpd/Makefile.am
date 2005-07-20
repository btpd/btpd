bin_PROGRAMS=btpd
btpd_SOURCES=\
	btpd.c btpd.h\
	cli_if.c\
	net.c net.h\
	queue.h \
	peer.c peer.h\
	policy_choke.c policy_if.c policy_subr.c policy.h\
	torrent.c torrent.h\
	tracker_req.c tracker_req.h

btpd_LDADD=../misc/libmisc.a -levent -lcrypto -lm
btpd_CPPFLAGS=-I$(top_srcdir)/misc @event_CPPFLAGS@ @openssl_CPPFLAGS@
btpd_CFLAGS=@CURL_CFLAGS@
btpd_LDFLAGS=@event_LDFLAGS@ @openssl_LDFLAGS@ @CURL_LDFLAGS@
