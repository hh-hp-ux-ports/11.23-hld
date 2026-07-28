# hld — a linker for HP-UX 11.23 / Itanium, LP64

`hld` is a from-scratch ELF-64 linker targeting **HP-UX 11i v2 (B.11.23) on
Itanium (IPF)**, **LP64 ABI** (the `/usr/lib/hpux64` world, ELF64 big-endian). The goal
is a drop-in replacement for `/usr/ccs/bin/ld` in an LP64 gcc toolchain on Integrity
hardware. hld is the build-time linker; the run-time side stays HP's dynamic loader
dld.so, whose loading contract hld's output must satisfy.

## Why

HP's `/usr/ccs/bin/ld` (92453-07 **B.12.34**, Feb 2006, with the last obtainable
cumulative linker patch installed) is the newest linker still reachable for 11.23 —
later toolset patches existed only in support archives that are gone. B.12.34 has three
independently root-caused relocation defects that block a modern gcc toolchain
(evidence in `docs/hp-ld-defects.md`):

1. **`R_IA64_GPREL64I` MLX bundle corruption** — slot 0 of the bundle is overwritten
   along with the L+X immediate slots, destroying whatever instruction was there.
   Every string-literal reference in gcc ≥ 4.9 code SIGILLs; working around it in the
   compiler (forcing the older `@ltoff` addressing) costs an extra load per reference.
2. **Out-of-range direct `br.call`** — in text larger than PCREL21B's ±16 MB reach
   (gcc 9.5's cc1plus: 37.6 MB), a call can get a direct branch to a bogus address
   instead of a long-branch stub. No workaround exists; this defect is the immediate
   motivation for the project.
3. **Unwind relocation `r_offset` handling** that disagrees with GNU gas output.

No free linker covers this target: GNU ld has **no `ia64-*-hpux*` emulation at all**
(BFD only carries object read/write vectors, for gas and the binary tools), gold never
had an ia64 port, and neither lld nor mold know ia64. See `docs/gnu-ld-status.md`.

## Target format

- ELF64, **big-endian** (`ELFDATA2MSB`), `EM_IA_64`, `ELFOSABI_HPUX`.
- Instruction bundles inside sections remain little-endian (ISA-fixed); all ELF
  metadata and data words are big-endian — relocation application must handle both.
- LP64 only in v1; ILP32 (`/usr/lib/hpux32`, ELF32 MSB) is out of scope until LP64
  works.

## Layout

```
src/        linker + ELF library sources (C99, no external deps)
tools/      standalone inspection tools (hld-readelf, ...)
tests/      test scripts; run against samples/
samples/    reference corpus from a real 11.23 system (samples/sys/ = HP system
            binaries, not tracked)
docs/       observed format findings, the ld invocation contract, defect dossier
scripts/    helpers (ground-truth collection on a live 11.23 system)
```

## Build & test

No autotools, no configure step, no external dependencies: one hand-written
Makefile that any POSIX make can run.

```
make CC=gcc                                  a typical Unix host
make CC="gcc -mlp64"                         natively on HP-UX 11.23/Itanium
make CC="aCC -Ae +DD64" CSTD= CWARN= COPT=-O natively with HP aC++
```

The last line matters: hld builds with the **vendor C compiler alone**, using
stock HP `make`, so it can be bootstrapped on a stock 11.23 system with no GNU
toolchain present — which is the situation this linker exists to improve. (The
bundled `/usr/bin/cc` is not an ANSI compiler and cannot be used; aC++ in `-Ae`
mode can.)

`make package` builds an SD depot for `swinstall`, carrying the binaries, an
`ld` alias that takes precedence over the system linker on PATH, and the
complete corresponding source — see `docs/packaging.md`.

`make check` runs the suites. The reader and relocation tests run anywhere; the
link tests need an `ia64-hp-hpux` assembler (point `XAS` at one, or put it in an
untracked `tests/local.conf` — see `tests/local.conf.example`); the dynamic tests
need HP-UX/Itanium itself, since only the real dynamic loader can judge them.
Each suite skips with a message rather than failing when its prerequisites are
absent.

## Status

Early, but it links real programs and they run.

**A compiled C program that calls `printf`, linked by hld against the system C
library, prints correctly and exits cleanly on HP-UX 11.23/Itanium.** Static
executables work too, and hld builds and runs natively on 11.23 — with GNU tools
or with the vendor compiler alone.

Working today:

- ELF64-MSB reader (`hld-readelf`), cross-validated against GNU readelf.
- IA-64 instruction-field relocation engine, golden-tested against assembler
  output — including the bundle shapes HP's linker corrupts.
- Section collection, address assignment, symbol resolution with COMMON (both
  the generic and IA-64 flavors), and the linker-defined symbol set.
- The DLT (GOT) and `.opd` function descriptors, so `LTOFF`/`FPTR` relocations
  and function pointers work.
- Static and dynamic `ET_EXEC` output: the loader handshake, shared libraries as
  input, imports through `.plt` descriptors with generated call stubs, exported
  dynamic symbols, and `DT_NEEDED`.

Not there yet: archives (`.a`), shared-library *output*, unwind-table merging
across objects, long-branch stubs for text beyond a branch's reach (the defect
that blocks the large compiler binaries this project is aimed at), TLS, and
debug-section handling. Anything hld cannot do yet is refused with a clear
message rather than mis-linked.

## License

GPLv3-or-later (see COPYING). `src/ia64_patch.c` is derived from GNU binutils
(`bfd/elfxx-ia64.c`, `bfd/cpu-ia64-opc.c`; Copyright Free Software Foundation,
contributed by David Mosberger-Tang). Vendoring policy: surgical and attributed —
that one battle-tested leaf only, no BFD/elflink framework code.
