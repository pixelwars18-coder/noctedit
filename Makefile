PREFIX   ?= /usr/local
CC       ?= cc
PKGS      = wayland-client wayland-cursor xkbcommon freetype2 fontconfig zlib
CFLAGS   ?= -O2 -pipe
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -fno-plt -D_FORTIFY_SOURCE=2 $(shell pkg-config --cflags $(PKGS)) -Ibuild
LDFLAGS  += -Wl,-O1,--as-needed,-z,relro,-z,now
LDLIBS    = $(shell pkg-config --libs $(PKGS)) -lm -ldl

WLP      := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
SCANNER  := $(shell pkg-config --variable=wayland_scanner wayland-scanner)

PROTOS = \
	$(WLP)/stable/xdg-shell/xdg-shell.xml \
	$(WLP)/stable/viewporter/viewporter.xml \
	$(WLP)/stable/tablet/tablet-v2.xml \
	$(WLP)/staging/cursor-shape/cursor-shape-v1.xml \
	$(WLP)/staging/fractional-scale/fractional-scale-v1.xml \
	$(WLP)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
	$(WLP)/unstable/primary-selection/primary-selection-unstable-v1.xml

PROTO_H = $(patsubst %.xml,build/%-client-protocol.h,$(notdir $(PROTOS)))
PROTO_C = $(patsubst %.xml,build/%-protocol.c,$(notdir $(PROTOS)))
SRC     = src/app.c src/doc.c src/draw.c src/syntax.c src/theme.c src/wl.c src/spell.c src/pdf.c src/docview.c
OBJ     = $(patsubst src/%.c,build/%.o,$(SRC)) $(PROTO_C:.c=.o)

all: noctedit

noctedit: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c src/noctedit.h $(PROTO_H) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: build/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

define proto_rules
build/$(basename $(notdir $(1)))-client-protocol.h: $(1) | build
	$(SCANNER) client-header $$< $$@
build/$(basename $(notdir $(1)))-protocol.c: $(1) | build
	$(SCANNER) private-code $$< $$@
endef
$(foreach p,$(PROTOS),$(eval $(call proto_rules,$(p))))

build:
	mkdir -p build

install: noctedit
	install -Dm755 noctedit $(DESTDIR)$(PREFIX)/bin/noctedit
	install -Dm644 data/noctedit.desktop $(DESTDIR)$(PREFIX)/share/applications/noctedit.desktop
	install -Dm644 data/noctedit.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/noctedit.svg
	-gtk-update-icon-cache -q -t $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null || true
	install -Dm644 data/noctalia/noctedit-colors.json $(DESTDIR)$(PREFIX)/share/noctedit/noctalia/noctedit-colors.json
	install -Dm644 data/noctalia/noctedit.toml $(DESTDIR)$(PREFIX)/share/noctedit/noctalia/noctedit.toml

# Hooks noctedit into Noctalia's template engine for the current user so the
# editor recolors live with the wallpaper/theme (same mechanism Noctalia uses for GTK, kitty, …).
install-noctalia-template:
	install -Dm644 data/noctalia/noctedit-colors.json $${XDG_CONFIG_HOME:-$$HOME/.config}/noctalia/templates/noctedit-colors.json
	install -Dm644 data/noctalia/noctedit.toml $${XDG_CONFIG_HOME:-$$HOME/.config}/noctalia/noctedit.toml
	@echo "Template installed. Change wallpaper/scheme once (or restart Noctalia) to generate the palette."

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/noctedit $(DESTDIR)$(PREFIX)/share/applications/noctedit.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/noctedit.svg
	rm -rf $(DESTDIR)$(PREFIX)/share/noctedit

clean:
	rm -rf build noctedit

.PHONY: all install install-noctalia-template uninstall clean

# Headless test build (no Wayland): scripted input + PNG snapshots
build/headless: tests/headless.c src/app.c src/doc.c src/draw.c src/syntax.c src/theme.c src/spell.c src/pdf.c src/docview.c src/noctedit.h | build
	$(CC) $(CFLAGS) -g -Dmain=app_main -c -o build/app_t.o src/app.c
	$(CC) $(CFLAGS) -g -o $@ tests/headless.c build/app_t.o src/doc.c src/draw.c src/syntax.c src/theme.c src/spell.c src/pdf.c src/docview.c \
		$(shell pkg-config --cflags --libs xkbcommon freetype2 fontconfig zlib) -lm -ldl
