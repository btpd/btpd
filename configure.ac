AC_PREREQ(2.61)
AC_INIT(btpd, 0.14+, btpd@murmeldjur.se)

AM_INIT_AUTOMAKE([1.10 foreign subdir-objects])

AC_CONFIG_FILES([Makefile])

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_RANLIB
AC_USE_SYSTEM_EXTENSIONS

AC_DEFUN([PROG_SILENT_IFELSE],
ac_output=`$1 2>&1`
if test x"$ac_output" != x; then
echo $1 nonsilent >&AS_MESSAGE_LOG_FD
echo $ac_output >&AS_MESSAGE_LOG_FD
[$3]
else
[$2]
fi)
AC_DEFUN([CC_ARGS_OK_IFELSE],
AC_LANG_PUSH(C)
AC_LANG_CONFTEST([int foo(void) { return 12; }])
AC_LANG_POP(C)
PROG_SILENT_IFELSE([$CC $1 -c conftest.c],
rm -f conftest.c conftest.$OBJEXT
[$2],
rm -f conftest.c conftest.$OBJEXT
[$3]))

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

old_LIBS=$LIBS
LIBS=""
AC_SEARCH_LIBS(inet_ntop, nsl,,AC_MSG_FAILURE(btpd needs inet_ntop))
AC_SEARCH_LIBS(bind, socket,,AC_MSG_FAILURE(btpd needs bind))
AC_SUBST(INETLIBS,$LIBS)
LIBS=$old_LIBS
AC_CHECK_FUNCS(asprintf)

AC_MSG_CHECKING(for CLOCK_MONOTONIC)
AC_COMPILE_IFELSE([
    #include <sys/time.h>
    #include <time.h>
    int main(void) { return clock_gettime(CLOCK_MONOTONIC, (void *)0); }
],  clock_gettime=yes, clock_gettime=no)
AC_MSG_RESULT($clock_gettime)
if test $clock_gettime == yes; then
    old_LIBS=$LIBS
    LIBS=""
    AC_SEARCH_LIBS(clock_gettime,rt,clock_gettime=yes,clock_gettime=no)
    AC_SUBST(CLOCKLIB,$LIBS)
    LIBS=$old_LIBS
    AC_DEFINE(HAVE_CLOCK_MONOTONIC)
fi
if test $clock_gettime == no; then
    AC_CHECK_FUNCS(mach_absolute_time,,
        AC_MSG_FAILURE(no supported time mechanism found))
fi

AC_MSG_CHECKING(whether compiler accepts -Wno-pointer-sign)
CC_ARGS_OK_IFELSE(-Wno-pointer-sign,
    AC_SUBST(WARNNPS,"-Wno-pointer-sign")
    AC_MSG_RESULT(yes)
,
    AC_MSG_RESULT(no))

old_LDFLAGS="$LDFLAGS"
LDFLAGS="$LDFLAGS $openssl_LDFLAGS"
AC_CHECK_LIB(crypto, SHA1_Final,:,AC_MSG_FAILURE(btpd needs openssl's libraries and headers))
LDFLAGS=$old_LDFLAGS

for m in $evloop_methods; do
    case $m in
    epoll)
        AC_CHECK_FUNC(epoll_wait,[
            AC_SUBST(EVLOOP_IMPL,"evloop/epoll.${OBJEXT}")
            AC_SUBST(EVLOOP_METHOD,EVLOOP_EPOLL)
            break])
        ;;
    kqueue)
        AC_CHECK_FUNC(kqueue,[
            AC_SUBST(EVLOOP_IMPL,"evloop/kqueue.${OBJEXT}")
            AC_SUBST(EVLOOP_METHOD,EVLOOP_KQUEUE)
            break])
        ;;
    poll)
        AC_CHECK_FUNC(poll,[
            AC_SUBST(EVLOOP_IMPL,"evloop/poll.${OBJEXT}")
            AC_SUBST(EVLOOP_METHOD,EVLOOP_POLL)
            break])
        break
        ;;
    *)
        AC_MSG_ERROR($m is not a valid evloop method)
        exit 1
        ;;
    esac
done
if test x"$EVLOOP_METHOD" == x; then
    AC_MSG_FAILURE(no suitable evloop method found)
    exit 1
else
    AC_MSG_NOTICE(selected evloop method $EVLOOP_METHOD)
fi

for i in 0 1 2 3 4 5 6; do
    case $i in
    0) if test x"$TD_CFLAGS$TD_LIBS" == x; then continue; fi;;
    1) TD_CFLAGS=-pthread;;
    2) TD_CFLAGS=-pthreads;;
    3) TD_CFLAGS=-kthread;;
    4) TD_CFLAGS=-mt;;
    5) TD_LIBS=-lpthread;;
    6) AC_MSG_FAILURE(how do you build with pthreads on this system?);;
    esac
    AC_MSG_CHECKING(whether pthreads compiles with flags \"$TD_CFLAGS $TD_LIBS\")
    CC_ARGS_OK_IFELSE($TD_CFLAGS $CFLAGS $TD_LIBS $LIBS,,
        AC_MSG_RESULT(no)
        TD_CFLAGS=""; TD_LIBS=""
        continue)

    old_CFLAGS=$CFLAGS; old_LIBS=$LIBS
    CFLAGS="$TD_CFLAGS $CFLAGS"; LIBS="$TD_LIBS $LIBS"
    AC_LINK_IFELSE([
        #include <pthread.h>
        int main(void) { return pthread_create(0,0,0,0);}
    ], [
        AC_MSG_RESULT(yes)
        CFLAGS=$old_CFLAGS; LIBS=$old_LIBS
        break
    ], [
        AC_MSG_RESULT(no)
        TD_CFLAGS=""; TD_LIBS=""
        CFLAGS=$old_CFLAGS; LIBS=$old_LIBS
    ])
done
AC_SUBST(TD_CFLAGS)
AC_SUBST(TD_LIBS)

AC_OUTPUT
