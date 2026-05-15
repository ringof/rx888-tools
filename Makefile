# rx888-tools Makefile — librx888 + binaries
#
# Replaces the previous Makefile. Two changes:
#   1. New target: librx888.so (+ static archive). Installs to LIBDIR.
#   2. rx888_stream now links against librx888 instead of building
#      its libusb code in-tree.
#
# Targets unchanged: rx888_dsp, iqrecord, install, uninstall, clean.

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBDIR     ?= $(PREFIX)/lib
INCDIR_INST?= $(PREFIX)/include
PCDIR      ?= $(LIBDIR)/pkgconfig
UDEVDIR    ?= /etc/udev/rules.d
FWDIR      ?= $(PREFIX)/share/rx888_tools/firmware
DOCDIR     ?= $(PREFIX)/share/doc/rx888_tools

CC         ?= gcc
INSTALL    := install

VERSION    := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

CFLAGS_COMMON := -std=c11 -O3 -Wall -Wextra -Wpedantic \
                 -fstack-protector-all -ggdb3 -fPIC \
                 -DVERSION=\"$(VERSION)\"

LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0)

CFLAGS_LIB    := $(CFLAGS_COMMON) -Werror -march=native -pthread $(LIBUSB_CFLAGS)
CFLAGS_STREAM := $(CFLAGS_COMMON) -pthread
CFLAGS_DSP    := $(CFLAGS_COMMON) -mavx2 -mfma -march=native
CFLAGS_REC    := $(CFLAGS_COMMON)

SRCDIR := src
INCDIR := include

# librx888 sources
LIBRX_OBJS := $(SRCDIR)/librx888.o $(SRCDIR)/ezusb.o
LIBRX_SO   := librx888.so
LIBRX_A    := librx888.a
LIBRX_HDR  := $(INCDIR)/librx888.h
LIBRX_PC   := librx888.pc

BINS := rx888_stream rx888_dsp iqrecord

all: $(LIBRX_SO) $(LIBRX_A) $(LIBRX_PC) $(BINS)

# --- librx888 ---
$(SRCDIR)/librx888.o: $(SRCDIR)/librx888.c $(INCDIR)/librx888.h $(INCDIR)/rx888.h $(INCDIR)/ezusb.h
	$(CC) $(CFLAGS_LIB) -I$(INCDIR) -c $< -o $@

$(SRCDIR)/ezusb.o: $(SRCDIR)/ezusb.c $(INCDIR)/ezusb.h
	$(CC) $(CFLAGS_LIB) -I$(INCDIR) -c $< -o $@

$(LIBRX_SO): $(LIBRX_OBJS)
	$(CC) -shared -Wl,-soname,$(LIBRX_SO) -o $@ $^ $(LIBUSB_LIBS) -lpthread

$(LIBRX_A): $(LIBRX_OBJS)
	ar rcs $@ $^

$(LIBRX_PC):
	@printf 'prefix=%s\n'                                                 $(PREFIX) > $@
	@printf 'exec_prefix=$${prefix}\n'                                            >> $@
	@printf 'libdir=%s\n'                                                 $(LIBDIR) >> $@
	@printf 'includedir=%s\n\n'                                       $(INCDIR_INST) >> $@
	@printf 'Name: librx888\n'                                                    >> $@
	@printf 'Description: Streaming library for the RX888 mk II SDR\n'            >> $@
	@printf 'Version: %s\n'                                              $(VERSION) >> $@
	@printf 'Requires.private: libusb-1.0\n'                                      >> $@
	@printf 'Libs: -L$${libdir} -lrx888\n'                                        >> $@
	@printf 'Cflags: -I$${includedir}\n'                                          >> $@

# --- binaries ---
rx888_stream: $(SRCDIR)/rx888_stream.c $(LIBRX_SO) $(LIBRX_HDR)
	$(CC) $(CFLAGS_STREAM) -I$(INCDIR) $(SRCDIR)/rx888_stream.c \
	    -L. -lrx888 -Wl,-rpath,'$$ORIGIN' -o $@

rx888_dsp: $(SRCDIR)/rx888_dsp.c
	$(CC) $(CFLAGS_DSP) -I$(INCDIR) $< -o $@ -lm -lpthread

iqrecord: $(SRCDIR)/iqrecord.c
	$(CC) $(CFLAGS_REC) -I$(INCDIR) $< -o $@

# --- install ---
install: all
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR_INST) $(DESTDIR)$(PCDIR)
	$(INSTALL) -m 755 $(LIBRX_SO) $(DESTDIR)$(LIBDIR)/
	$(INSTALL) -m 644 $(LIBRX_A)  $(DESTDIR)$(LIBDIR)/
	$(INSTALL) -m 644 $(LIBRX_HDR) $(DESTDIR)$(INCDIR_INST)/
	$(INSTALL) -m 644 $(LIBRX_PC) $(DESTDIR)$(PCDIR)/
	$(INSTALL) -d $(DESTDIR)$(BINDIR) && $(INSTALL) -m 755 $(BINS) $(DESTDIR)$(BINDIR)/
	$(INSTALL) -d $(DESTDIR)$(FWDIR) && $(INSTALL) -m 644 firmware/SDDC_FX3.img $(DESTDIR)$(FWDIR)/
	$(INSTALL) -d $(DESTDIR)$(UDEVDIR) && $(INSTALL) -m 644 udev/99-rx888.rules $(DESTDIR)$(UDEVDIR)/
	-ldconfig 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(LIBRX_SO) $(DESTDIR)$(LIBDIR)/$(LIBRX_A)
	rm -f $(DESTDIR)$(INCDIR_INST)/librx888.h $(DESTDIR)$(PCDIR)/$(LIBRX_PC)
	rm -f $(addprefix $(DESTDIR)$(BINDIR)/,$(BINS))
	rm -f $(DESTDIR)$(FWDIR)/SDDC_FX3.img
	rm -f $(DESTDIR)$(UDEVDIR)/99-rx888.rules

clean:
	rm -f $(BINS) $(LIBRX_SO) $(LIBRX_A) $(LIBRX_PC) $(SRCDIR)/*.o

.PHONY: all install uninstall clean
