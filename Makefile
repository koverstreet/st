# st - simple terminal
# See LICENSE file for copyright and license details.

# st version
VERSION = 0.3

PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man
X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib
GSETTINGS_SCHEMAS = /usr/share/glib-2.0/schemas

CFLAGS		:= -MD -MP -g -std=gnu99 -O2 -Wall -Wextra -Werror	\
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

INSTALL		:= install
INSTALL_PROGRAM	:= ${INSTALL}
INSTALL_DATA	:= ${INSTALL} -m 0644

all: st

OBJS = st.o term.o
DEP_FILES := $(wildcard *.d)

-include $(DEP_FILES)

st: $(OBJS)
st.o: config.h

config.h:
	cp config.def.h config.h

clean:
	$(RM) st $(OBJS) $(DEP_FILES)

install: all
	$(INSTALL_PROGRAM) -t $(DESTDIR)$(PREFIX)/bin st
	$(INSTALL_DATA) -t $(DESTDIR)$(MANPREFIX)/man1 st.1
	$(INSTALL_DATA) -t $(DESTDIR)$(GSETTINGS_SCHEMAS) org.evilpiepirate.st.gschema.xml
	glib-compile-schemas $(DESTDIR)$(GSETTINGS_SCHEMAS)

uninstall:
	$(RM) ${DESTDIR}${PREFIX}/bin/st
	$(RM) ${DESTDIR}${MANPREFIX}/man1/st.1
	$(RM) $(DESTDIR)$(GSETTINGS_SCHEMAS)/org.evilpiepirate.st.gschema.xml
	glib-compile-schemas $(DESTDIR)$(GSETTINGS_SCHEMAS)

.PHONY: clean install uninstall
