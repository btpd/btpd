AC_INIT(btpd, 0.13+, btpd@murmeldjur.se)

AM_INIT_AUTOMAKE([foreign subdir-objects])

AC_CONFIG_FILES([Makefile])

AC_PROG_CC
AC_PROG_CC_C_O
AC_PROG_RANLIB

AC_ARG_WITH(openssl,
[  --with-openssl=dir      use openssl installed in dir],
[
    AC_SUBST(openssl_LDFLAGS,["-L${withval}/lib -Wl,-R,${withval}/lib"])
    AC_SUBST(openssl_CPPFLAGS,"-I${withval}/include")
])

AC_ARG_WITH(evloop-method,
[  --with-evloop-method    select evloop method (epoll,poll,kqueue)],
    evloop_methods=$withval,
    evloop_methods="epoll kqueue poll")

AC_SEARCH_LIBS(clock_gettime, rt,
    AC_SUBST(CLOCKLIB, $LIBS)
    LIBS=""
,
    echo btpd needs clock_gettime.
    exit 1)

echo -n "checking whether compiler accepts -Wno-pointer-sign... "
old_CFLAGS=$CFLAGS
CFLAGS="-Wno-pointer-sign"
AC_COMPILE_IFELSE(int main(void) { return 0; },
    AC_SUBST(WARNNPS,"-Wno-pointer-sign")
    echo yes
,
    echo no)
CFLAGS=$old_CFLAGS

old_LDFLAGS="$LDFLAGS"
LDFLAGS="$LDFLAGS $openssl_LDFLAGS"
AC_CHECK_LIB(crypto, SHA1_Final,, echo "btpd needs openssl's libraries and headers."; exit 1)
LDFLAGS=$old_LDFLAGS
LIBS=""

for m in $evloop_methods; do
    case $m in
    epoll)
        AC_CHECK_FUNC(epoll_wait,[
            AC_SUBST(EVLOOP_IMPL,"evloop/epoll.${OBJEXT}")
            AC_SUBST(EVLOOP_METHOD,EVLOOP_EPOLL)
            break],)
        ;;
    kqueue)
        AC_CHECK_FUNC(kqueue,[
            AC_SUBST(EVLOOP_IMPL,"evloop/kqueue.${OBJEXT}")
            AC_SUBST(EVLOOP_METHOD,EVLOOP_KQUEUE)
            break],)
        ;;
    poll)
        AC_CHECK_FUNC(poll,[
            AC_SUBST(EVLOOP_IMPL,"evloop/poll.${OBJEXT}")
            AC_SUBST(EVLOOP_METHOD,EVLOOP_POLL)
            break],)
        break
        ;;
    *)
        echo "$m is not a valid evloop method. See --help."
        exit 1
        ;;
    esac
done
if test x$EVLOOP_METHOD == x; then
    echo "no suitable evloop method found."
    exit 1
else
    echo selected evloop method $EVLOOP_METHOD.
fi

AC_OUTPUT
