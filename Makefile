# btpd - BitTorrent Protocol Daemon
# See LICENSE file for copyright and license details.

BTPD_SRC    = ${wildcard btpd/*.c}
BTPD_DEPS   = ${wildcard btpd/*.h}
BTPD_OBJ    = ${BTPD_SRC:.c=.o}

BTCLI_SRC   = ${wildcard cli/*.c}
BTCLI_DEPS  = ${wildcard cli/*.h}
BTCLI_OBJ   = ${BTCLI_SRC:.c=.o}

BTINFO_SRC  = ${wildcard info/*.c}
BTINFO_DEPS = ${wildcard info/*.h}
BTINFO_OBJ  = ${BTINFO_SRC:.c=.o}

MISC_SRC    = ${wildcard misc/*.c}
MISC_DEPS   = ${wildcard misc/*.h}
MISC_OBJ    = ${MISC_SRC:.c=.o}

EVLOOP_SRC  = ${wildcard evloop/*.c}
EVLOOP_DEPS = ${wildcard evloop/*.h}
EVLOOP_OBJ  = ${EVLOOP_SRC:.c=.o}

include config.mk

all: options btpd/btpd info/btinfo cli/btcli

options:
	@echo btpd build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	${CC} -c ${DEFS} ${CPPFLAGS} ${CFLAGS} $< -o $@

${%_OBJ}: ${%_DEPS}

misc/libmisc.a: ${MISC_OBJ}
	ar rcs $@ ${MISC_OBJ}

evloop/libevloop.a: ${EVLOOP_OBJ}
	ar rcs $@ ${EVLOOP_OBJ}

btpd/btpd: ${BTPD_OBJ} misc/libmisc.a evloop/libevloop.a
	${CC} ${CFLAGS} -o $@ ${BTPD_OBJ}	 misc/libmisc.a evloop/libevloop.a ${LDFLAGS}

info/btinfo: ${BTINFO_OBJ} misc/libmisc.a
	${CC} ${CFLAGS} -o $@ ${BTINFO_OBJ} misc/libmisc.a ${LDFLAGS}

cli/btcli: ${BTCLI_OBJ} misc/libmisc.a
	${CC} ${CFLAGS} -o $@  ${BTCLI_OBJ}  misc/libmisc.a ${LDFLAGS}

clean:
	rm -f btpd/btpd cli/btcli info/btinfo\
		**/*.o **/*.a\
		btpd-${VERSION}.tar.gz

dist: clean
	mkdir -p btpd-${VERSION}
	cp -R COPYRIGHT Makefile README CHANGES configure config.mk btpd cli doc evloop info misc\
		btpd-${VERSION}
	tar -cf btpd-${VERSION}.tar btpd-${VERSION}
	gzip btpd-${VERSION}.tar
	rm -rf btpd-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f btpd/btpd cli/btcli info/btinfo ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/btpd
	chmod 755 ${DESTDIR}${PREFIX}/bin/btcli
	chmod 755 ${DESTDIR}${PREFIX}/bin/btinfo
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	cp -f doc/*.1 ${DESTDIR}${MANPREFIX}/man1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/btpd.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/btcli.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/btinfo.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/btpd\
		${DESTDIR}${PREFIX}/bin/btcli\
		${DESTDIR}${PREFIX}/bin/btinfo\
		${DESTDIR}${MANPREFIX}/man1/btpd.1\
		${DESTDIR}${MANPREFIX}/man1/btcli.1\
		${DESTDIR}${MANPREFIX}/man1/btinfo.1

.PHONY: all options clean dist install uninstall
