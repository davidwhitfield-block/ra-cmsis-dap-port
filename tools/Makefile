# Host-side tools for the RA4M2 CMSIS-DAP probe.
#
#   brew install libusb        # macOS
#   sudo apt install libusb-1.0-0-dev   # Debian/Ubuntu
#   make && ./daptest all

CC      ?= cc
# Appended, not assigned: the environment often already carries -I/-L for a
# package manager prefix, and ?= would silently drop the warning flags.
CFLAGS  += -O2 -Wall -Wextra -std=gnu11
LDLIBS  := -lusb-1.0 -lm

# Homebrew keeps headers and libs outside the default search paths.
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW_PREFIX),)
CFLAGS  += -I$(BREW_PREFIX)/include
LDFLAGS += -L$(BREW_PREFIX)/lib
endif

BINS := daptest

all: $(BINS)

daptest: daptest.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(BINS)

.PHONY: all clean
