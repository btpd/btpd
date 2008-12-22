AC_INIT(btpd, 0.13, btpd@murmeldjur.se)

AM_INIT_AUTOMAKE([foreign])

AC_CONFIG_FILES([Makefile btpd/Makefile misc/Makefile cli/Makefile])

AC_PROG_CC
AC_PROG_RANLIB

CFLAGS="$CFLAGS -std=c99"
CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE=1 -D_FILE_OFFSET_BITS=64"

AC_ARG_WITH(openssl,
[  --with-openssl=dir      use openssl installed in dir],
[
        AC_SUBST(openssl_LDFLAGS,["-L${withval}/lib -Wl,-rpath=${withval}/lib"])
        AC_SUBST(openssl_CPPFLAGS,"-I${withval}/include")
],
[])

AC_ARG_WITH(warn,
[  --with-warn=level       select warning preset (no,all,allerr)],
[
    case $withval in
    no)
	;;
    all)
	CFLAGS="$CFLAGS -Wall -Wno-pointer-sign"
	;;
    allerr)
	CFLAGS="$CFLAGS -Wall -Wno-pointer-sign -Werror"
	;;
    *)
	echo "Warning preset \"$withval\" not recognized. See --help."
	exit 1
	;;
    esac
],
[])

old_LDFLAGS="$LDFLAGS"
LDFLAGS="$LDFLAGS $openssl_LDFLAGS"
AC_CHECK_LIB(crypto, SHA1_Final, :, echo Must have openssl; exit 1)
LDFLAGS=$old_LDFLAGS

AC_CONFIG_SUBDIRS([libevent])

AC_OUTPUT
