AUTOMAKE_OPTIONS = foreign no-dependencies


EXTRA_DIST = acconfig.h event.h event-internal.h log.h evsignal.h \
	kqueue.c epoll_sub.c epoll.c select.c rtsig.c poll.c signal.c \
	evport.c devpoll.c buffer.c \
	compat/sys/queue.h compat/sys/tree.h compat/sys/_time.h \
	WIN32-Code/config.h WIN32-Code/misc.c \
	WIN32-Code/win32.c WIN32-Code/misc.h

noinst_LIBRARIES = libevent.a

if BUILD_WIN32

SYS_LIBS = -lws2_32
SYS_SRC = WIN32-Code/misc.c WIN32-Code/win32.c
SYS_INCLUDES = -I$(top_srcdir)/WIN32-Code

else

SYS_LIBS =
SYS_SRC =
SYS_INCLUDES =

endif

libevent_a_SOURCES = event.c buffer.c event.h log.c evdns.c evdns.h $(SYS_SRC)
libevent_a_LIBADD = @LIBOBJS@ $(SYS_LIBS)

INCLUDES = -I$(top_srcdir)/compat $(SYS_INCLUDES)

DISTCLEANFILES = *~
