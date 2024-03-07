SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

CFLAGS=-Wall -Werror -Wextra -Wpedantic -Wno-unused-parameter -Wconversion $\
	-Wformat-security -Wformat -Wsign-conversion -Wfloat-conversion $\
	-Wunused-result $(shell pkg-config --cflags pixman-1)
LIBS=-lwayland-client $(shell pkg-config --libs pixman-1) -lrt
OBJ=wayneko.o wlr-layer-shell-unstable-v1.o xdg-shell.o ext-idle-notify-v1.o
GEN=wlr-layer-shell-unstable-v1.c wlr-layer-shell-unstable-v1.h $\
	xdg-shell.c xdg-shell.h $\
	ext-idle-notify-v1.c ext-idle-notify-v1.h

wayneko: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

%.c: %.xml
	$(SCANNER) private-code < $< > $@

%.h: %.xml
	$(SCANNER) client-header < $< > $@

install: wayneko
	install        -D wayneko   $(DESTDIR)$(BINDIR)/wayneko
	install -m 644 -D wayneko.1 $(DESTDIR)$(MANDIR)/man1/wayneko.1

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/wayneko
	$(RM) $(DESTDIR)$(MANDIR)/man1/wayneko.1

clean:
	$(RM) wayneko $(GEN) $(OBJ)

.PHONY: clean install

