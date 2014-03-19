# st - simple terminal
# See LICENSE file for copyright and license details.

# st version
VERSION = 0.3

PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man
X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

CFLAGS		:= -g -std=gnu99 -O2 -Wall -Wextra -Werror	\
	-Wno-sign-compare					\
	-Wno-unused-parameter
CPPFLAGS	:= -DVERSION=\"${VERSION}\"			\
	-D_BSD_SOURCE -D_XOPEN_SOURCE=600			\
	-I. -I/usr/include -I${X11INC} 				\
	$(shell pkg-config --cflags fontconfig) 		\
	$(shell pkg-config --cflags freetype2)			\
	$(shell pkg-config --cflags gio-2.0)
LDFLAGS 	:= -g -L/usr/lib -L$(X11LIB)
LDLIBS		:= -lm -lc -lX11 -lutil -lXft			\
	 $(shell pkg-config --libs fontconfig)			\
	 $(shell pkg-config --libs freetype2)			\
	 $(shell pkg-config --libs gio-2.0)

OBJS = st.o term.o
DEP_FILES := $(wildcard *.d)

all: st
st: $(OBJS)

#ifneq ($(DEP_FILES),)
#	-include $(DEP_FILES)
#endif

%.o %.d: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MD -MP -MF $*.d -c $< -o $*.o

config.h:
	cp config.def.h config.h

clean:
	$(RM) st $(OBJS) $(DEP_FILES)

install: all
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f st ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/st
	echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < st.1 > ${DESTDIR}${MANPREFIX}/man1/st.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/st.1
	echo Please see the README file regarding the terminfo entry of st.
	tic -s st.info

uninstall:
	$(RM) ${DESTDIR}${PREFIX}/bin/st
	$(RM) ${DESTDIR}${MANPREFIX}/man1/st.1

.PHONY: clean install uninstall
