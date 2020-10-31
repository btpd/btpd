# btpd version
NAME = btpd
VERSION = 0.16

# paths
PREFIX = /usr
MANPREFIX = ${PREFIX}/share/man

MISC = ./misc
EVLOOP = ./evloop

# includes and libs
INCS = -I${MISC} -I${EVLOOP}
LIBS = -lm -lpthread

# flags
CPPFLAGS = ${INCS} -DHAVE_CLOCK_MONOTONIC=1 -DEVLOOP_POLL
CFLAGS = -march=native -pipe -O3 -fno-math-errno
LDFLAGS = ${LIBS}
DEFS = -DPACKAGE_NAME=\"${NAME}\" -DPACKAGE_VERSION=\"${VERSION}\"

# compiler
CC = gcc

# excluded
EVLOOP_SRC := ${filter-out evloop/epoll.c evloop/kqueue.c, ${EVLOOP_SRC}}
