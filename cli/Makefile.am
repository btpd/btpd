bin_PROGRAMS=btinfo btcli

btinfo_SOURCES=btinfo.c
btinfo_LDADD=../misc/libmisc.a -lcrypto -lm
btinfo_CPPFLAGS=-I$(top_srcdir)/misc @openssl_CPPFLAGS@
btinfo_LDFLAGS=@openssl_LDFLAGS@

btcli_SOURCES=btcli.c btpd_if.c btpd_if.h
btcli_LDADD=../misc/libmisc.a -lcrypto -lm
btcli_CPPFLAGS=-I$(top_srcdir)/misc @openssl_CPPFLAGS@
btcli_LDFLAGS=@openssl_LDFLAGS@
