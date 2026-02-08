# rx888_tools — RX888 SDR streaming pipeline
#
# Targets:
#   all            Build all binaries (default)
#   rx888_stream   USB3 capture → stdout
#   rx888_dsp      DSP decimation (stdin → stdout)
#   iqrecord       IQ recorder (stdin → SigMF files)
#   install        Install to PREFIX (default /usr/local)
#   uninstall      Remove installed files
#   clean          Remove build artifacts

# ----------------------------- Configuration ----------------------------- #

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
UDEVDIR    ?= /etc/udev/rules.d
FWDIR      ?= $(PREFIX)/share/rx888_tools/firmware
DOCDIR     ?= $(PREFIX)/share/doc/rx888_tools

CC         ?= gcc
INSTALL    := install

# Version: set from git tag if available, otherwise "dev"
VERSION    := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

# Common flags
CFLAGS_COMMON := -std=c11 -O3 -Wall -Wextra -Wpedantic \
                 -fstack-protector-all -ggdb3 \
                 -DVERSION=\"$(VERSION)\"

# Per-target flags
CFLAGS_STREAM := $(CFLAGS_COMMON) -Werror -march=native -pthread \
                 $(shell pkg-config --cflags libusb-1.0)
LIBS_STREAM   := $(shell pkg-config --libs libusb-1.0)

CFLAGS_DSP    := $(CFLAGS_COMMON) -mavx2 -mfma -march=native
LIBS_DSP      := -lm -lpthread

CFLAGS_REC    := $(CFLAGS_COMMON)
LIBS_REC      :=

# Paths
SRCDIR     := src
INCDIR     := include

# ------------------------------ Build rules ------------------------------ #

BINS := rx888_stream rx888_dsp iqrecord

all: $(BINS)

rx888_stream: $(SRCDIR)/rx888_stream.c $(SRCDIR)/ezusb.c \
              $(INCDIR)/ezusb.h $(INCDIR)/rx888.h
	$(CC) $(CFLAGS_STREAM) -I$(INCDIR) \
	    $(SRCDIR)/rx888_stream.c $(SRCDIR)/ezusb.c \
	    -o $@ $(LIBS_STREAM)

rx888_dsp: $(SRCDIR)/rx888_dsp.c
	$(CC) $(CFLAGS_DSP) -I$(INCDIR) \
	    $(SRCDIR)/rx888_dsp.c \
	    -o $@ $(LIBS_DSP)

iqrecord: $(SRCDIR)/iqrecord.c
	$(CC) $(CFLAGS_REC) -I$(INCDIR) \
	    $(SRCDIR)/iqrecord.c \
	    -o $@ $(LIBS_REC)

# ----------------------------- Install / remove -------------------------- #

install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(BINS) $(DESTDIR)$(BINDIR)/
	$(INSTALL) -d $(DESTDIR)$(FWDIR)
	$(INSTALL) -m 644 firmware/SDDC_FX3.img $(DESTDIR)$(FWDIR)/
	$(INSTALL) -d $(DESTDIR)$(UDEVDIR)
	$(INSTALL) -m 644 udev/99-rx888.rules $(DESTDIR)$(UDEVDIR)/
	@echo ""
	@echo "Installed to $(DESTDIR)$(PREFIX)"
	@echo "  Binaries:  $(DESTDIR)$(BINDIR)/"
	@echo "  Firmware:  $(DESTDIR)$(FWDIR)/"
	@echo "  udev rule: $(DESTDIR)$(UDEVDIR)/99-rx888.rules"
	@echo ""
	@echo "You may need to reload udev rules:"
	@echo "  sudo udevadm control --reload-rules && sudo udevadm trigger"

uninstall:
	rm -f $(addprefix $(DESTDIR)$(BINDIR)/,$(BINS))
	rm -f $(DESTDIR)$(FWDIR)/SDDC_FX3.img
	-rmdir $(DESTDIR)$(FWDIR) 2>/dev/null || true
	rm -f $(DESTDIR)$(UDEVDIR)/99-rx888.rules
	@echo "Uninstalled from $(DESTDIR)$(PREFIX)"

clean:
	rm -f $(BINS) *.o

.PHONY: all install uninstall clean
