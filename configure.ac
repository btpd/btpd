AC_INIT(btpd, 0.2, btpd@murmeldjur.se)

AC_CANONICAL_TARGET

AM_INIT_AUTOMAKE([foreign])

AC_CONFIG_FILES([Makefile btpd/Makefile misc/Makefile cli/Makefile])

AC_PROG_CC
AC_PROG_RANLIB

CFLAGS="$CFLAGS -std=c99 -Wall -Werror"

case $target_os in
     linux*)
	CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE=1 -D_FILE_OFFSET_BITS=64"
	;;
esac

AC_ARG_WITH(event,
[  --with-event=dir        use libevent installed in dir],
[
        AC_SUBST(event_LDFLAGS,["-L${withval}/lib -Wl,-rpath=${withval}/lib"])
        AC_SUBST(event_CPPFLAGS,"-I${withval}/include")
],
[])

AC_ARG_WITH(ssl,
[  --with-ssl=dir          use openssl installed in dir],
[
        AC_SUBST(openssl_LDFLAGS,["-L${withval}/lib -Wl,-rpath=${withval}/lib"])
        AC_SUBST(openssl_CPPFLAGS,"-I${withval}/include")
],
[])

AC_ARG_WITH(curlconf,
[  --with-curlconf=prog    use this curl-config],
[
    CURLCONF=$withval
],
[])

old_LDFLAGS="$LDFLAGS"
LDFLAGS="$LDFLAGS $event_LDFLAGS"
AC_CHECK_LIB(event, event_init, :, echo Must have libevent; exit 1)
LDFLAGS=$old_LDFLAGS

old_LDFLAGS="$LDFLAGS"
LDFLAGS="$LDFLAGS $openssl_LDFLAGS"
AC_CHECK_LIB(crypto, SHA1_Final, :, echo Must have openssl; exit 1)
LDFLAGS=$old_LDFLAGS

if test x$CURLCONF = x; then
   AC_PATH_PROG(CURLCONF, curl-config)
fi

if test x$CURLCONF = x -o \! \( -r "$CURLCONF" -a -x "$CURLCONF" \); then
   echo Must have the curl-config script
   exit 1
else
   AC_SUBST(CURL_CFLAGS, `$CURLCONF --cflags`)
   AC_SUBST(CURL_LDFLAGS, `$CURLCONF --libs`)
fi

old_LDFLAGS="$LDFLAGS"
LDFLAGS="$LDFLAGS $CURL_LDFLAGS"
AC_CHECK_LIB(curl, curl_easy_strerror, :, echo Must have recent curl; exit 1)
LDFLAGS=$old_LDFLAGS

AC_OUTPUT
