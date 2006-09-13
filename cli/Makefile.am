bin_PROGRAMS=btinfo btcli

btinfo_SOURCES=btinfo.c
btinfo_LDADD=../misc/libmisc.a -lcrypto -lm
btinfo_CPPFLAGS=-I$(top_srcdir)/misc @openssl_CPPFLAGS@
btinfo_LDFLAGS=@openssl_LDFLAGS@

btcli_SOURCES=btcli.c btcli.h add.c del.c list.c kill.c start.c stop.c stat.c
btcli_LDADD=../misc/libmisc.a -lcrypto -lm
btcli_CPPFLAGS=-I$(top_srcdir)/misc @openssl_CPPFLAGS@
btcli_LDFLAGS=@openssl_LDFLAGS@
