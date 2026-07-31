# hld 0.9.4 — an LP64 linker for HP-UX 11.23 on Itanium

A from-scratch ELF-64 linker for **HP-UX 11i v2 (B.11.23) on Integrity**, LP64 ABI
— the `/usr/lib/hpux64`, ELF64 big-endian world. It is the build-time linker; the
run-time side stays HP's `dld.so`, whose loading contract hld's output satisfies.

## Why this exists

`/usr/ccs/bin/ld` B.12.34 (Feb 2006, with the last obtainable cumulative linker
patch) is the newest linker reachable for 11.23, and it has three independently
root-caused relocation defects that block a modern gcc. No free linker covers
this target: GNU ld has no `ia64-*-hpux*` emulation at all, gold never had an
ia64 port, and neither lld nor mold know ia64.

## What this release does

**It links gcc 9.5's compilers, and they work.** `cc1`, `cc1plus` and `lto1` —
several hundred objects and archives each, up to 29 MB of text — and
`cc1`/`cc1plus` compile to assembly **byte-identical** to the same compilers
linked by the system linker.

The defect was measured directly rather than inferred. Disassembling both
binaries and counting `br.call` instructions whose target lands past `_etext`:

| binary | text | system linker | hld |
|---|---|---|---|
| `cc1` | 28.9 MB | **50** | **0** |
| `cc1plus` | 29.2 MB | **26** | **0** |
| `lto1` | 25.4 MB | **17** | **0** |

End to end: the libstdc++ source file that first exposed the defect fails with
`internal compiler error: Segmentation fault` under the compiler the system
linker produced, and compiles cleanly under the one hld produced — same source,
same flags, same compiler, different linker.

## Working

- Static and dynamic `ET_EXEC`, and shared libraries (`-b` / `-shared`) with a
  soname.
- Archives (`.a`) searched in command-line order, with `--start-group`; objects
  and archives from **both** gcc and the vendor compiler.
- The DLT (GOT), `.opd` descriptors, imports through `.plt` with generated call
  stubs, and the dynamic relocations that let the loader place a module wherever
  it likes.
- Thread-local storage, initializer/finalizer arrays, and one merged sorted
  unwind table — enough for C++ exceptions end to end.
- Long-branch stubs for calls beyond a direct branch's ±16 MB reach.
- Debug information carried through and relocated, so a debugger can set
  breakpoints by source line and walk a stack.
- Builds with the **vendor C compiler alone** (`aCC -Ae +DD64`) under stock HP
  `make`, so it can be bootstrapped on a system with no GNU toolchain present.

## Fixed since 0.9.3

- **Long-branch stub sets converge.** A call now reuses any stub it can reach
  rather than only the one in the nearest island. Inserting a stub moves
  addresses, so "nearest" changed between passes and each pass added another —
  one call could produce eight stubs and never settle.
- **A section longer than a branch's reach gets an island at each end.** An
  input section cannot be split, so a call at the far end of a large one
  previously had nothing in range behind it.
- **Every direct call is checked against the sections the image actually maps.**
  A target that is wrong but happens to fall within reach encodes cleanly and
  faults only when taken; that now fails the link, naming the symbol, the
  resolved address and the call site.
- **Linked output records the linker that produced it**, in the platform's
  `what` format.

## Known limitations

- `.gnu.linkonce` / COMDAT duplicates are kept rather than discarded — correct
  output, larger text.
- No local symbols in the output symbol table (debug information covers
  source-level debugging).
- Thread-local storage in a shared library (the dynamic-TLS relocations) is
  refused with a clear message.
- ILP32 is out of scope.

Input hld cannot handle is refused with a message rather than mis-linked.

## Installing

The depot is a gzipped serial SD depot. Uncompress it to **local disk** first —
`swinstall` cannot read a gzipped depot, and hangs on an NFS source:

```sh
/usr/contrib/bin/gzip -dc hld-0.9.4-ia64-11.23.sd.gz > /var/tmp/hld.sd
```

Then, as root:

```sh
swinstall -s /var/tmp/hld.sd HLD.RUN
```

That installs `hld` and `hld-readelf` under `/opt/hld/bin` and adds it to
`/etc/PATH`. The system linker is untouched.

To use it with gcc — which finds `ld` through `COMPILER_PATH`, not `PATH`:

```sh
mkdir -p /var/tmp/hldbin && ln -sf /opt/hld/bin/hld /var/tmp/hldbin/ld
```

and pass `-B/var/tmp/hldbin/` on the link line.

Two further filesets are available: `HLD.LDOVR` puts an `ld` ahead of
`/usr/ccs/bin` on the system PATH (reversible with `swremove HLD.LDOVR`), and
`HLD.SRC` installs the complete corresponding source under `/opt/hld/src`.
`swinstall -s /var/tmp/hld.sd HLD` takes all three.

## Which linker built a binary

```
$ what /opt/hld/bin/hld
        hld 0.9.4 - LP64 linker for HP-UX 11.23/IPF
```

The same string is present in anything hld links, in a non-loaded section.

## Building from source

No autotools, no configure, no dependencies — one hand-written Makefile any
POSIX make can run:

```sh
make CC="gcc -mlp64"
```

`make check` runs the suites (211 checks; the link and dynamic suites need an
`ia64-hp-hpux` assembler and the target platform respectively, and skip with a
message elsewhere).

## License

GPLv3-or-later. `src/ia64_patch.c` is derived from GNU binutils
(`bfd/elfxx-ia64.c`, `bfd/cpu-ia64-opc.c`; Copyright Free Software Foundation,
contributed by David Mosberger-Tang). The depot's `HLD.SRC` fileset carries the
complete corresponding source.
