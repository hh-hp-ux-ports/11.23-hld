# hld — builds with any C99 compiler and any POSIX make. No autotools, no
# GNU make extensions, no external dependencies.
#
#   make CC=gcc                     typical Unix host
#   make CC="gcc -mlp64"            natively on HP-UX 11.23/Itanium
#   make CC="aCC -Ae +DD64" CWARN=  natively with HP aC++
#
# The bundled HP cc is not a C99 compiler, so CC must be set there. Override
# CWARN (and CSTD) when the compiler does not take gcc's flags.

CC      = cc
CSTD    = -std=c99
CWARN   = -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes
COPT    = -O2 -g
CFLAGS  = $(CSTD) $(CWARN) $(COPT)

BUILD   = build

READER  = src/elfread.c
LINKSRC = src/hld.c src/link.c src/write.c src/dynamic.c src/archive.c src/ia64_patch.c $(READER)
HDRS    = src/elf64.h src/port.h src/ia64_patch.h src/link.h

all: $(BUILD)/hld $(BUILD)/hld-readelf $(BUILD)/patch_harness

$(BUILD)/hld: $(LINKSRC) $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(LINKSRC)

$(BUILD)/hld-readelf: tools/hld_readelf.c $(READER) $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/hld_readelf.c $(READER)

$(BUILD)/patch_harness: tests/patch_harness.c $(READER) src/ia64_patch.c $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/patch_harness.c $(READER) src/ia64_patch.c

# The reader and relocation suites run anywhere; the link suites need an ia64
# assembler, and the dynamic suite needs HP-UX/Itanium itself. Each skips with
# a message rather than failing when its prerequisites are absent.
check: all
	sh tests/check_readelf.sh
	sh tests/check_patch.sh
	sh tests/check_link.sh
	sh tests/check_archive.sh
	sh tests/check_dynamic.sh

# An SD depot for swinstall. Must be built on HP-UX (swpackage lives there);
# works as an ordinary user. Ships the binaries, an `ld` alias that takes
# precedence over the system linker on PATH, and the corresponding source.
VERSION = 0.1

package: all
	sh scripts/mkdepot.sh $(VERSION)

clean:
	rm -rf $(BUILD)
