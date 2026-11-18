# dnaren - minimal DNA renderer
# See LICENSE file for copyright and license details.
# Screenshot: https://raw.githubusercontent.com/tinyopsec/assets/main/dnaren/dnaren.png

VERSION = 0.1

PREFIX  = /usr/local
BINDIR  = ${PREFIX}/bin
MANPREFIX = ${PREFIX}/share/man
MANDIR  = ${MANPREFIX}/man1

X11INC  = /usr/X11R6/include
X11LIB  = /usr/X11R6/lib

# Linux
#X11INC = /usr/include/X11
#X11LIB = /usr/lib

# FreeBSD / DragonFly
#X11INC = /usr/local/include
#X11LIB = /usr/local/lib

# NetBSD (pkgsrc)
#X11INC = /usr/pkg/include
#X11LIB = /usr/pkg/lib

INCS     = -I${X11INC}
LIBS     = -L${X11LIB} -lX11 -lGL -lGLU -lm

CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_POSIX_C_SOURCE=200809L \
           -DVERSION=\"${VERSION}\"
CFLAGS   = -std=c99 -pedantic -Wall -Wextra \
           -Wstrict-prototypes -Wmissing-prototypes -Wshadow \
           -fno-common -O2 -flto ${INCS} ${CPPFLAGS}
LDFLAGS  = -flto ${LIBS}

# CLI build (terminal renderer, no X11/GL)
CLI_CFLAGS = -std=c99 -pedantic -Wall -Wextra \
             -Wstrict-prototypes -Wmissing-prototypes -Wshadow \
             -fno-common -O2 -flto ${CPPFLAGS}
CLI_LDFLAGS = -lm

# Debug: make debug
DBGFLAGS = -std=c99 -pedantic -Wall -Wextra \
           -Wstrict-prototypes -Wmissing-prototypes -Wshadow \
           -fno-common -g -O0 ${INCS} ${CPPFLAGS}

CC ?= cc

SRC = dnaren.c
OBJ = ${SRC:.c=.o}

CLI_SRC = dnaren-cli.c

all: dnaren cli

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: dnaren.h

dnaren: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

cli: dnaren-cli

dnaren-cli: ${CLI_SRC} dnaren.h
	${CC} ${CLI_CFLAGS} -o $@ ${CLI_SRC} ${CLI_LDFLAGS}

debug: ${SRC} dnaren.h
	${CC} ${DBGFLAGS} -o dnaren_dbg ${SRC} ${LIBS}

debug-cli: ${CLI_SRC} dnaren.h
	${CC} -std=c99 -pedantic -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wshadow -fno-common -g -O0 ${CPPFLAGS} -o dnaren-cli_dbg ${CLI_SRC} -lm

dist: clean
	mkdir -p dnaren-${VERSION}
	cp -R dnaren.c dnaren.h dnaren.1 dnaren-cli.c Makefile LICENSE README.md dnaren-${VERSION}
	tar -czf dnaren-${VERSION}.tar.gz dnaren-${VERSION}
	rm -rf dnaren-${VERSION}

clean:
	rm -f dnaren dnaren_dbg ${OBJ} dnaren-cli dnaren-cli_dbg dnaren-${VERSION}.tar.gz

install: all
	mkdir -p ${DESTDIR}${BINDIR}
	install -m 755 dnaren ${DESTDIR}${BINDIR}/dnaren
	install -m 755 dnaren-cli ${DESTDIR}${BINDIR}/dnaren-cli
	mkdir -p ${DESTDIR}${MANDIR}
	sed "s/VERSION/${VERSION}/g" < dnaren.1 > ${DESTDIR}${MANDIR}/dnaren.1
	chmod 644 ${DESTDIR}${MANDIR}/dnaren.1

uninstall:
	rm -f ${DESTDIR}${BINDIR}/dnaren ${DESTDIR}${BINDIR}/dnaren-cli ${DESTDIR}${MANDIR}/dnaren.1

.PHONY: all clean debug dist install uninstall cli debug-cli