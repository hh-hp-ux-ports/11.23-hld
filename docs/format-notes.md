# HP-UX 11.23 / IPF LP64 ELF — format notes

Everything here is **observed** from real HP-UX 11.23/Itanium build output (a small
reference corpus in `samples/gt1/`, generated as described in `scripts/recon.sh`), not
copied from documentation. Producer of record: HP ld B.12.34 fed by gcc 4.7.4 + GNU as
2.18; system objects by HP's own toolchain.

## Identification

| field | value |
|---|---|
| e_ident | `7f 45 4c 46 02 02 01 01 01` — ELF64, **2's-complement big-endian**, EV_CURRENT, OSABI **1 = HP-UX**, **ABIVERSION 1** |
| e_machine | 50 (EM_IA_64) |
| e_flags | `.o`: **0x18** = EF_IA_64_BE (0x8) \| EF_IA_64_ABI64 (0x10). HP-ld exec/DSO: **0x19** (adds 0x1 = TRAPNIL, the `-z` nulptr bit). dld.so itself: 0x18 |
| e_type | REL / EXEC / DYN as usual; **dld.so is ET_EXEC** with vaddr 0 text, 0x80000000 data |
| e_phentsize/e_shentsize | 56 / 64 (standard ELF64) |

Instruction bundles inside sections are **little-endian** (ISA-fixed 16-byte bundles);
every other multi-byte value in the file is big-endian.

## Address space / segment layout (LP64)

- Text LOAD: vaddr `0x4000000000000000`, R E, p_align small (0x10 in a minimal
  executable; libc shows 0x8000000000000000 — align field is evidently advisory here).
- Data LOAD: vaddr `0x6000000000000000`, RW. File offset starts at the next 0x10000
  boundary — offset ≡ vaddr (mod 0x10000).
- p_paddr always 0.
- Program header vocabulary observed (a minimal dynamic executable, 12 entries, in
  file order): `PT_PHDR, PT_INTERP, PT_DYNAMIC, PT_NOTE(.note.hpux_options),
  PT_IA_64_UNWIND (0x70000001), PT_HP_TLS (0x60000000), PT_HP_LINKER_FOOTPRINT
  (0x60000016, → unmapped .note), PT_LOAD(text), PT_LOAD(data), PT_HP_FASTBIND
  (0x60000011, → .fastbind), PT_NOTE(unmapped, memsz 0), PT_HP_STACK (0x60000014, RW,
  size 0)`. DSOs: same minus INTERP/FASTBIND/STACK. Empty HP_TLS phdr is present even
  with no TLS; libc's HP_TLS covers its `.tbss` (vaddr in data segment, memsz 0x80).
- **PT_INTERP string is a colon-separated list**: `/usr/lib/hpux64/uld.so:/usr/lib/hpux64/dld.so`.

## Section inventory of linked outputs

Text segment (in address order): `.note.hpux_options`(NOTE) `.interp` `.dynamic`
`.dynsym` `.dynstr` `.hash`(SHT_HASH, sh_link 0!) `.rela.plt` `.IA_64.unwind_hdr`
`.IA_64.unwind` (SHT_IA_64_UNWIND, entsize 4 in linked output, sh_info = .text index)
`.IA_64.unwind_info` `.rodata` `.dynhash`(**PROGBITS** — HP global-hash format, not
SHT_HASH) `.opd` `.text` `.bortext` (the lazy-binding trampoline — see below, *not* branch
stubs, despite the name; `_etext`/`_etext_f` come after it).

Data segment: `.data` `.HP.init` `.HP.preinit` `.init_array` `.preinit_array`
`.fini_array` `.plt`(entsize 0x10) `.dlt`(entsize 8) `.sdata` `.sbss` `.bss` `.hbss`
`.tbss`. Short-data sections (`.plt .dlt .sdata .sbss`) carry SHF_IA_64_SHORT
(0x10000000, readelf flag `p`).

Unmapped: `.fastbind` `.note`(linker footprint) `.comment` `.strtab` `.symtab` `.shstrtab`.

In DSOs `.opd` sits in the **data** segment (function descriptors for address-taken
functions); in minimal samples it is empty.

## The linkage model (IPF-specific)

- **`.dlt` is the GOT** ("data linkage table"), addressed gp-relative. **DT_PLTGOT holds
  the gp value**, i.e. `__gp`, anchored at `.sdata` start so the ±2 MB addl window
  covers `.plt`/`.dlt`/`.sdata`/`.sbss` around it.
- **`.plt` holds 16-byte import descriptors {fptr, gp}** in the *data* segment. Import
  binding is via `.rela.plt` entries of type **R_IA64_IPLTMSB** against the UND symbol
  (one 16-byte slot each). No code `.plt` exists; call-side import stubs are ordinary
  text the linker emits (load descriptor from .plt slot, `mov b6`, `ld8 gp`, `br`).
- **DT_IA_64_PLT_RESERVE** names a 3-slot (24-byte) region reserved for the dynamic
  loader.
- Exec `.dynsym`: UND imports are **WEAK FUNC with st_value = the address the symbol
  resolved to at link time inside the needed library** (fastbind-style precomputed
  hint — e.g. `printf` carries its libc.so.1 address). Also exports `main` and the
  linker-defined symbols. libc exports are largely `PROTECTED`.
- Data imports/exports go through DLT slots — dynamic reloc `R_IA64_DIR64MSB` on the
  `.dlt` slot. **No copy relocations on this platform.**
- Dynamic relocation sections are **per-target-section**: `.rela.plt`, `.rela.dlt`,
  `.rela.sdata`, `.rela.opd`, `.rela.data`, `.rela.init_array` (libc uses the full set).

## Dynamic section (observed tag set)

Standard: NEEDED*, SONAME(DSO), **RUNPATH** (HP ld embeds the `-L` list by default!),
FLAGS (libc: STATIC_TLS), PLTGOT(=gp), HASH, STRTAB, SYMTAB, RELA/RELASZ/RELAENT,
PLTREL=RELA, JMPREL, PLTRELSZ, STRSZ, SYMENT, INIT_ARRAY/INIT_ARRAYSZ (when nonempty),
NULL. Plus:

| tag | value seen | reading |
|---|---|---|
| 0x70000000 DT_IA_64_PLT_RESERVE | start..end range | dynamic-loader-reserved DLT triple |
| 0x60000000 | nonzero, exec only | HP_LOAD_MAP — bss slot the loader fills |
| 0x60000001 | 0 (exec/so), 8 (libc) | HP_DLD_FLAGS |
| 0x60000008 | looks like a build-time Unix timestamp | HP_TIME_STAMP |
| 0x60000009 | pseudo-random | HP_CHECKSUM |
| 0x6000000a | matches chatr's reported size, exec only | global hash table size |
| 0x6000000b | 1 | global-hash/gst version |
| 0x6000000c | → `.dynhash` vaddr | global hash table address |
| 0x6000000d/0x6000000e | libc only | unknown pair (investigate if the loader cares) |

## Relocations

**In gcc 4.7.4 `-mlp64` objects (the input set hld must handle first):**
R_IA64_PCREL21B (0x49, calls), R_IA64_GPREL22 (0x2a, short data), R_IA64_LTOFF22X (0x86)
+ R_IA64_LDXMOV (0x87) pairs (linker-relaxable DLT access — treating 22X as plain
LTOFF22 and LDXMOV as no-op is always legal), R_IA64_SEGREL64MSB (0x5e, unwind tables),
plus from gcc 9.5 the R_IA64_GPREL64I (0x2b) on MLX bundles (see `hp-ld-defects.md`),
DIR64MSB (0x26) in data, FPTR/LTOFF_FPTR variants for address-taken functions,
PCREL64I for long branches.

**In linked outputs (dynamic):** R_IA64_IPLTMSB (0x80, function-descriptor import),
R_IA64_DIR64MSB (0x26, DLT/data slots), R_IA64_FPTR64MSB (0x46, a descriptor the loader
must make). One HP dynamic reloc type, **0x82**, appears in libc.so.1 and is absent from
the public GNU relocation-numbering headers; it **writes a 16-byte `{entry, gp}`
descriptor**, established by reading what the loader's own `.opd` holds at the addresses
it names. It pairs positionally with `DT_HP_EPLTREL`, so an EPLT/export-descriptor form
is the likely reading.

## What the kernel's loader actually requires (static executables)

Determined by mutating a known-good executable one field at a time and
running each variant — not from documentation. A minimal static ET_EXEC with
**only** `PT_PHDR` + `PT_LOAD`(text) + `PT_LOAD`(data) loads and runs; every
HP-specific program header is optional:

| feature | required? | evidence |
|---|---|---|
| `PT_INTERP` / `.dynamic` / dynamic loader | **no** | neutralizing PT_INTERP in a working binary still runs; a purely static image runs |
| `.note.hpux_options` + its `PT_NOTE` | no | neutralized, still runs |
| `PT_HP_TLS`, `PT_HP_FASTBIND`, `PT_HP_STACK`, `PT_HP_LINKER_FOOTPRINT`, unmapped `PT_NOTE` | no | each neutralized individually, still runs |
| **a writable LOAD segment** | **YES** | dropping it (leaving PHDR + text LOAD, `e_phnum` 2) → `Exec format error`, even though nothing needs writable memory |
| **every LOAD's `p_offset` within the file** | **YES** | an otherwise-valid image whose (empty) data segment pointed past EOF → `Exec format error`; padding the file to cover that offset made the identical image run |

The second rule is easy to violate accidentally: `p_filesz == 0` does *not*
excuse an out-of-range `p_offset`. hld therefore always extends the output
file to at least `data_off + data_filesz`.

## What the dynamic loader requires

Established the same way — removing one `.dynamic` entry at a time from a
working dynamic executable and running the result. Removal of a required tag
segfaults inside the loader before `main`.

| tag | image with no imports | image importing from a library |
|---|---|---|
| `DT_HASH` | **required** | **required** |
| `DT_HP_LOAD_MAP` | **required** | **required** |
| `DT_STRTAB`, `DT_SYMTAB`, `DT_PLTGOT`, `DT_RELA`, `DT_JMPREL`, `DT_IA_64_PLT_RESERVE`, `DT_HP_DLD_FLAGS` | optional | **required** |
| `DT_STRSZ`, `DT_SYMENT`, `DT_RELASZ`, `DT_RELAENT`, `DT_PLTREL`, `DT_PLTRELSZ` | optional | optional |
| `DT_FLAGS`, `DT_RUNPATH`, `DT_HP_TIME_STAMP`, `DT_HP_CHECKSUM`, `DT_HP_GST_SIZE`, `DT_HP_GST_VERSION`, `DT_HP_GST_HASHVAL` | optional | optional |

The size tags being optional while the tables themselves are mandatory
suggests the loader walks the linkage tables rather than the relocation
arrays for lazy binding. hld emits the full set regardless.

`DT_HP_LOAD_MAP` and `DT_IA_64_PLT_RESERVE` both point at writable scratch
the loader owns — a single word and a 24-byte (three-slot) region
respectively, in short bss.

### What the executable must provide for a library to load

The C library binds to the executable, not just the other way round. Its
dynamic symbol table lists `main`, `_end`, `__ARGC`, `__ARGV`, `__ENVP`,
`__LOAD_INFO`, `__SYSTEM_ID_D` and `__TLS_SIZE_D` as undefined; the loader
supplies the argument/load-info group itself, but `main` and `_end` must be
**exported by the executable** or loading fails with "Unsatisfied data
symbol". An executable therefore has to emit real exports in `.dynsym`, and
the linker's own symbols are part of that set.

Loadable segments must also carry the HP-specific `p_flags` bits — text
`PF_HP_CODE`, data `PF_HP_MODIFY` (the platform's linker also sets
`PF_HP_LAZYSWAP` and bit `0x20000` on text) — and `PT_INTERP` must precede
the loadable segments.

### Advertising the import relocations

`DT_RELA` and `DT_JMPREL` may name the same relocation array, which is what
the platform's linker does when binding is deferred. Under **immediate
binding** that arrangement makes the loader traverse the array twice and
reject the image ("not a valid load module"); advertising it once through
`DT_RELA` works. The import descriptor should also be pre-filled
(`{entry point, gp}`) rather than left zero, and imported symbols are marked
weak.

### Addresses that belong to another module

A reference to a shared library is not always a call. A linkage-table slot can
hold the address of imported *data*, and a data word can hold such an address
outright — C++ start-up does both, reaching the C library's `__iob` (the `FILE`
table behind `stdout`), `__SB_masks` and `errno` that way. Neither address
exists at link time, so each needs a dynamic relocation for the loader to apply:

| what holds the address | relocation |
|---|---|
| linkage-table slot naming imported data | `R_IA64_DIR64MSB` |
| linkage-table slot naming an imported function | `R_IA64_FPTR64MSB` |
| data word holding an imported address | `R_IA64_DIR64MSB` |
| import descriptor in `.plt` | `R_IA64_IPLTMSB` |

`FPTR64` asks the loader for the *canonical* descriptor: the entry point and the
gp both belong to the defining module, so the linker cannot construct one.

The platform's linker keeps these in per-target sections (`.rela.dlt`,
`.rela.sdata`, `.rela.init_array`, …) laid out contiguously, and pre-fills a
plain-address slot with the link-time binding while leaving a descriptor slot
zero. hld emits a single `.rela.dyn` instead, which the loader sees identically
because it reads the array through `DT_RELA`.

Getting this wrong is quiet: the slot or word keeps whatever the linker left
there, and the program faults inside the runtime's own start-up rather than at
the reference that was mis-resolved.

### Calls beyond a direct branch's reach

`R_IA64_PCREL21B` carries 21 bits of bundle-granular displacement: ±16 MB. A
larger text — a C++ compiler's is more than twice that — means some calls cannot
be encoded at all and have to be routed through a stub near the caller. A single
bundle of `brl.cond.sptk.many` does it: a 60-bit displacement, no register
clobbered, and `b0` untouched, so the callee still returns to the original
caller. `brl` is an Itanium 2 instruction and every machine running this ABI
has it.

Placement is the real constraint: one stub section at the end of the image is
itself unreachable from the front of a large text, so stubs have to be spread
through the code at intervals below the branch's reach. Code is not all in
`.text` either — a C++ compiler emits thousands of one-function
`.gnu.linkonce.t.*` sections, and a call from any of them may need a stub.

(HP's `.bortext` is *not* this. It holds the lazy-binding trampoline: one
`mov r15=<index>; br.few <resolver>` per import, plus the resolver preamble
that loads the entry point and gp from the descriptor.)

### Calling an imported function

The compiler emits a plain `R_IA64_PCREL21B` direct call to the external
symbol; the linker redirects it to a generated stub and puts a 16-byte
`{entry point, gp}` import descriptor in `.plt`, with an `R_IA64_IPLTMSB`
relocation in `.rela.plt` naming the symbol. The stub is three bundles:

```
    addl    r15 = <plt slot - gp>, r1   // address the descriptor gp-relatively
    ;;
    ld8.acq r16 = [r15], 8              // entry point, then step to the gp word
    mov     r14 = r1                    // current gp (used by the lazy resolver)
    ;;
    ld8     r1 = [r15]                  // the callee's gp
    mov     b6 = r16
    br.few  b6
```

Before binding, the descriptor's entry word points at a loader trampoline
that records which slot was called; after binding it holds the real address.
Undefined dynamic symbols also carry the address the symbol resolved to at
link time in `st_value`, as a hint.

**A static executable is entered with `gp` == 0** — the kernel does not set it
up (probed by exiting with the top nibble of `r1`). Only the dynamic loader
sets `gp`, from `DT_PLTGOT`, before transferring control. Statically linked
code that uses gp-relative addressing must therefore establish `gp` itself
(`movl gp = __gp`), the job a startup file does on other platforms. This does
not affect dynamically linked programs, which is what a real toolchain
produces.

Note that HP ld emits a dynamic executable (PT_INTERP, `.dynamic`, `.dynsym`,
`.hash`, `.dynhash`) even for an object that references nothing outside
itself — that is its policy, not a platform requirement.

## Startup contract

- **Dynamic executable e_entry = `main` directly.** No crt0 code is linked; the dynamic
  loader + libc perform all startup (crt0.o exists only for archive links, which are
  impossible for LP64 libc — there is **no /usr/lib/hpux64/libc.a**; `-static` fails
  with "Can't find library or mismatched ABI for -lc").
- Entry registers (recovered on the ILP32 ABI, expected identical on LP64): r32=argc
  r33=argv r34=envp r35=load-info block, gp from `__gp`. Entry is a code address, not a
  descriptor.
- **Syscall gateway, LP64-confirmed** by disassembling libc's `_exit`: syscall number
  in **r8** (exit=1), also `mov r9=2` (role not yet determined), gateway table base in
  **ar.k7**, entry = `ld8 [k7 + 8*nr]`, call `br.call b6=b7` (link register **b6**),
  success predicate **p15**; error path calls a stub then sets r8=-1. Args per normal
  C ABI outs.

## Linker-defined symbols (must-provide set)

`__text_start`, `__text_start_f`, `_DYNAMIC`, `__unwind_header`, `_etext`, `_etext_f`,
`__data_start`, `__hp_preinit_start/__hp_preinit_end`, `__init_start/__init_end`,
`__fini_start/__fini_end`, `__gp`, `_edata`, `_end` (+`_end_f`?), and the unwind trio
bounds. (`__xpg4_extended_mask` comes from `unix98.o`, not the linker.)

## Misc

- `.note.hpux_options` (NOTE, 0x78 bytes) leads the text segment — decode before
  emitting our own.
- HP ld emits `.fastbind` (+ PT_HP_FASTBIND) even though chatr reports fastbind
  disabled; hypothesis: omission is tolerated. Verify once dynamic-executable output
  exists.
- `.hbss` = huge-bss (empty in samples). `.HP.opt_annot` (readelf shows the type as
  "VMS_LINKAGES" = 0x60000004) appears in HP-cc objects — pass-through/ignorable for v1.
- HP cc debug sections in system objects: `.debug_actual`, `.debug_procs_info`, etc. —
  non-alloc, pass through or drop like any debug section.
