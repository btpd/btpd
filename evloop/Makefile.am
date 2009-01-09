noinst_LIBRARIES=libevloop.a
EXTRA_libevloop_a_SOURCES=epoll.c poll.c
libevloop_a_SOURCES=\
	evloop.h\
	timeheap.c timeheap.h timer.c
CFLAGS=@CFLAGS@ -D@EVLOOP_METHOD@ -I$(top_srcdir)/misc
libevloop_a_LIBADD=@EVLOOP_IMPL@
libevloop_a_DEPENDENCIES=@EVLOOP_IMPL@
