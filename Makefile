# hld — builds with any C99 host compiler, and natively on HP-UX 11.23/Itanium.
# Keep flags conservative: C99, no GNU extensions, no external deps.
# HP-UX native build: gmake CC='gcc -mlp64'

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes
BUILD    = build

TOOLS    = $(BUILD)/hld $(BUILD)/hld-readelf $(BUILD)/patch_harness
READER_SRC = src/elfread.c
LINK_SRC = src/hld.c src/link.c src/write.c src/dynamic.c src/ia64_patch.c $(READER_SRC)
HDRS     = src/elf64.h src/port.h src/ia64_patch.h src/link.h

all: $(TOOLS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/hld: $(LINK_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(LINK_SRC)

$(BUILD)/hld-readelf: tools/hld_readelf.c $(READER_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/hld_readelf.c $(READER_SRC)

$(BUILD)/patch_harness: tests/patch_harness.c $(READER_SRC) src/ia64_patch.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/patch_harness.c $(READER_SRC) src/ia64_patch.c

check: $(TOOLS)
	sh tests/check_readelf.sh
	sh tests/check_patch.sh
	sh tests/check_link.sh

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
