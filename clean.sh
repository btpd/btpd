#! /bin/sh

set -x

true \
&& rm -f aclocal.m4 \
&& rm -f -r autom4te.cache \
&& rm -f compile \
&& rm -f config.guess \
&& rm -f config.log \
&& rm -f config.status \
&& rm -f config.sub \
&& rm -f configure \
&& rm -f depcomp \
&& rm -f install-sh \
&& rm -f -r libltdl \
&& rm -f libtool \
&& rm -f ltmain.sh \
&& rm -f Makefile \
&& rm -f Makefile.in \
&& rm -f missing \
&& rm -f evloop/*.o \
&& rm -f evloop/*.a \
&& rm -f evloop/*.la \
&& rm -f evloop/*.lo \
&& rm -f btpd/*.o \
&& rm -f btpd/*.a \
&& rm -f btpd/*.la \
&& rm -f btpd/*.lo \
&& rm -f cli/*.o \
&& rm -f cli/*.a \
&& rm -f cli/*.la \
&& rm -f cli/*.lo \
&& rm -f misc/*.o \
&& rm -f misc/*.a \
&& rm -f misc/*.la \
&& rm -f misc/*.lo \
&& rm -f btpd/btpd \
&& rm -f cli/btcli \
&& rm -f cli/btinfo \
&& rm -f btpd/config.h \
&& rm -f btpd/config.h.in \
&& rm -f btpd/config.h.in~ \
&& rm -f btpd/Makefile \
&& rm -f btpd/Makefile.in \
&& rm -f cli/config.h \
&& rm -f cli/config.h.in \
&& rm -f cli/config.h.in~ \
&& rm -f cli/Makefile \
&& rm -f cli/Makefile.in \
&& rm -f evloop/config.h \
&& rm -f evloop/config.h.in \
&& rm -f evloop/config.h.in~ \
&& rm -f evloop/Makefile \
&& rm -f evloop/Makefile.in \
&& rm -f misc/config.h \
&& rm -f misc/config.h.in \
&& rm -f misc/config.h.in~ \
&& rm -f misc/Makefile \
&& rm -f misc/Makefile.in \
&& rm -rf cli/.deps/ \
&& rm -rf cli/.dirstamp \
&& rm -rf misc/.deps/ \
&& rm -rf misc/.dirstamp \
&& rm -rf btpd/.deps/ \
&& rm -rf btpd/.dirstamp \
&& rm -rf evloop/.deps/ \
&& rm -rf evloop/.dirstamp

