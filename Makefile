CC       ?= cc
CFLAGS   ?= -Wall -Wextra -O2
CFLAGS   += -DWLR_USE_UNSTABLE -Iprotocols $(shell pkg-config --cflags wlroots-0.20 wayland-server xkbcommon)
LDLIBS   := $(shell pkg-config --libs wlroots-0.20 wayland-server xkbcommon) -lm
WAYLAND_SCANNER ?= $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)

PREFIX      ?= /usr/local
BINDIR      := $(PREFIX)/bin
SHAREDIR    := $(PREFIX)/share
WSESSIONDIR := $(SHAREDIR)/wayland-sessions

TARGET   := canvas
SRCS     := canvas.c config.c toml.c
OBJS     := $(SRCS:.c=.o)

# Vendored toml.c/toml.h stay in-tree on purpose (no external lib dep).
# Wayland protocol header is generated at build time, never committed.
PROTOCOL_XML := protocols/wlr-layer-shell-unstable-v1.xml
PROTOCOL_H   := protocols/wlr-layer-shell-unstable-v1-protocol.h

all: $(TARGET)

$(PROTOCOL_H): $(PROTOCOL_XML)
	$(WAYLAND_SCANNER) server-header $< $@

# Build the protocol header before any object (safe under -j).
$(OBJS): $(PROTOCOL_H)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c config.h toml.h
	$(CC) $(CFLAGS) -c -o $@ $<

toml.o: toml.c toml.h
	$(CC) $(CFLAGS) -Wno-discarded-qualifiers -c -o $@ $<

install: all
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -dm755 $(DESTDIR)$(WSESSIONDIR)
	sed -e 's|^Exec=.*|Exec=$(BINDIR)/$(TARGET)|' \
	    -e 's|^TryExec=.*|TryExec=$(BINDIR)/$(TARGET)|' \
	    $(TARGET).desktop > $(DESTDIR)$(WSESSIONDIR)/$(TARGET).desktop
	chmod 644 $(DESTDIR)$(WSESSIONDIR)/$(TARGET).desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(WSESSIONDIR)/$(TARGET).desktop

clean:
	rm -f $(TARGET) $(OBJS) $(PROTOCOL_H)

.PHONY: all clean install uninstall
