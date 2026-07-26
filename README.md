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

`make` builds the tools with the host compiler; `make check` runs the test suites.
The code is C99 with no dependencies and also builds natively on HP-UX 11.23
(`gcc -mlp64 -std=c99`). Some tests want an `ia64-hp-hpux` cross gas/readelf
(binutils 2.46.x): point `XAS`/`CROSS_READELF` at them, or drop the definitions into
an untracked `tests/local.conf`; without them those tests skip. Linked outputs are
ultimately validated by running them on real Integrity hardware.

## Status

Early, but it links and the results run.

**hld produces working static LP64 executables.** A hand-written assembly object
linked by hld into an ET_EXEC — segments, program headers, symbol resolution,
linker-defined symbols and all — executes correctly on HP-UX 11.23/Itanium and
returns its expected exit status. hld also builds and runs natively on 11.23, where
it links working binaries on the machine itself.

Working today: the ELF64-MSB reader (`hld-readelf`, cross-validated against GNU
readelf), the IA-64 instruction-field relocation engine (golden-tested against gas
encodings, including the bundle shapes HP ld corrupts), section collection and
layout, symbol resolution with COMMON allocation, and static ET_EXEC output.

Not there yet: archives and libraries, the DLT (GOT) and function-descriptor
construction that `LTOFF`/`FPTR` relocations need, dynamic executables (the dld.so
contract), shared libraries, unwind-table merging, and long-branch stubs.
Relocations that need machinery hld does not have yet are rejected with a clear
message rather than mis-applied.

## License

GPLv3-or-later (see COPYING). `src/ia64_patch.c` is derived from GNU binutils
(`bfd/elfxx-ia64.c`, `bfd/cpu-ia64-opc.c`; Copyright Free Software Foundation,
contributed by David Mosberger-Tang). Vendoring policy: surgical and attributed —
that one battle-tested leaf only, no BFD/elflink framework code.
