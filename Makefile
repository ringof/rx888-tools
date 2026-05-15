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

# Firmware is pinned to a tag of ringof/rx888-firmware.  The blob is
# fetched on demand via `make firmware` (or `make hw-check`) and is
# checksum-verified.  CI bumps the tag via `make firmware-latest`.
RX888_FW_REPO := ringof/rx888-firmware
RX888_FW_TAG  := $(strip $(shell cat firmware/VERSION 2>/dev/null))
RX888_FW_FILE := firmware/SDDC_FX3.img
RX888_FW_URL  := https://github.com/$(RX888_FW_REPO)/releases/download/$(RX888_FW_TAG)/SDDC_FX3-$(RX888_FW_TAG).img

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

# --- non-hardware tests (CI) ---
TESTS_DIR  := tests
TEST_BINS  := $(TESTS_DIR)/librx888_api

$(TESTS_DIR)/librx888_api: $(TESTS_DIR)/librx888_api.c $(LIBRX_SO) $(LIBRX_HDR)
	$(CC) $(CFLAGS_STREAM) -I$(INCDIR) $(LIBUSB_CFLAGS) $< \
	    -L. -lrx888 -Wl,-rpath,'$$ORIGIN/..' -o $@

check: $(TEST_BINS) rx888_stream
	$(TESTS_DIR)/librx888_api
	$(TESTS_DIR)/cli_smoke.sh ./rx888_stream

# --- firmware (pinned to rx888-firmware release) ---
firmware: $(RX888_FW_FILE)

$(RX888_FW_FILE): firmware/VERSION firmware/SHA256SUMS
	@[ -n "$(RX888_FW_TAG)" ] || { echo "firmware/VERSION is empty"; exit 1; }
	@echo "Fetching firmware $(RX888_FW_TAG) from $(RX888_FW_REPO)..."
	@curl -fL --retry 3 -o $@.tmp $(RX888_FW_URL)
	@cd firmware && sha256sum -c SHA256SUMS.tmp 2>/dev/null || true
	@mv $@.tmp $@
	@cd firmware && sha256sum -c SHA256SUMS

# Bump VERSION + SHA256SUMS to whatever rx888-firmware tagged most
# recently.  Used by the auto-bump CI workflow; not part of normal
# developer flow.  Requires curl + jq + sha256sum.
firmware-latest:
	@command -v jq >/dev/null || { echo "jq required"; exit 2; }
	@tag=$$(curl -fsSL https://api.github.com/repos/$(RX888_FW_REPO)/releases/latest | jq -r .tag_name); \
	  [ -n "$$tag" ] && [ "$$tag" != "null" ] || { echo "could not resolve latest tag"; exit 1; }; \
	  echo "Bumping firmware to $$tag"; \
	  url="https://github.com/$(RX888_FW_REPO)/releases/download/$$tag/SDDC_FX3-$$tag.img"; \
	  curl -fL -o firmware/SDDC_FX3.img.tmp "$$url"; \
	  echo "$$tag" > firmware/VERSION; \
	  ( cd firmware && sha256sum SDDC_FX3.img.tmp \
	      | sed 's| .*$$|  SDDC_FX3.img|' > SHA256SUMS ); \
	  mv firmware/SDDC_FX3.img.tmp $(RX888_FW_FILE)

# --- hardware tests (require RX888 + RX888_HW_TEST=1) ---
hw-check: all $(RX888_FW_FILE)
	RX888_STREAM=$(CURDIR)/rx888_stream RX888_FW=$(CURDIR)/$(RX888_FW_FILE) \
	  $(TESTS_DIR)/hw_smoke.sh
	RX888_STREAM=$(CURDIR)/rx888_stream RX888_FW=$(CURDIR)/$(RX888_FW_FILE) \
	  $(TESTS_DIR)/hw_stop_start.sh
	RX888_STREAM=$(CURDIR)/rx888_stream RX888_FW=$(CURDIR)/$(RX888_FW_FILE) \
	  $(TESTS_DIR)/hw_sample_check.py

.PHONY: check firmware firmware-latest hw-check

# --- install ---
install: all
	@[ -f $(RX888_FW_FILE) ] || { \
	  echo "Firmware blob $(RX888_FW_FILE) is not present."; \
	  echo "Run 'make firmware' to fetch it from rx888-firmware $(RX888_FW_TAG)."; \
	  exit 2; }
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR_INST) $(DESTDIR)$(PCDIR)
	$(INSTALL) -m 755 $(LIBRX_SO) $(DESTDIR)$(LIBDIR)/
	$(INSTALL) -m 644 $(LIBRX_A)  $(DESTDIR)$(LIBDIR)/
	$(INSTALL) -m 644 $(LIBRX_HDR) $(DESTDIR)$(INCDIR_INST)/
	$(INSTALL) -m 644 $(LIBRX_PC) $(DESTDIR)$(PCDIR)/
	$(INSTALL) -d $(DESTDIR)$(BINDIR) && $(INSTALL) -m 755 $(BINS) $(DESTDIR)$(BINDIR)/
	$(INSTALL) -d $(DESTDIR)$(FWDIR) && $(INSTALL) -m 644 $(RX888_FW_FILE) $(DESTDIR)$(FWDIR)/
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
	rm -f $(TEST_BINS)

.PHONY: all install uninstall clean
