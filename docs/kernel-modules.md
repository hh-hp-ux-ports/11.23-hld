# Kernel modules (DLKMs) and relocatable output

An HP-UX 11.23 loadable kernel module is a **relocatable object** (ET_REL),
not an executable: the module's objects are combined with `ld -r`, installed
under `/usr/conf/mod`, and the kernel's module loader applies the relocations
itself when `kcmodule` loads it. This note records what HP ld -r produces,
what a module has to contain, and how `hld -r` compares. Everything marked
**measured** was observed on the 11.23 build box (rx2620) on 2026-10-09;
**inferred** means reasoned from measurements, not tested by loading.

The linker measured is the one on that box: `92453-07 linker ld HP
Itanium(R) B.12.42 IPF/IPF` (`ld -V`, and the footprint it writes into every
output) -- newer than the B.12.34 named in `hp-ld-defects.md`.

## How a module is built (the HP recipe)

From gavial's `guest/gvtape` (loaded on 11.23 and 11.31 guests) and
hp-ux-driver-development's `hpext2`:

```
cc +DD64 -D_KERNEL -I/usr/include/.unsupp -Wc,-codeconvention,esnonreloc -c x.c
cc ... -S modmeta.c; rename .data/.sdata to .data.KC_META/.data.KC_META_PTR; as +DD64
as +DD64 -o shtmod.o shtmod.s            # the .shtmod record, hand-written
ld -r -o module x.o wrapper.o modmeta.o shtmod.o
patch_shtmod.py module                   # .shtmod -> SHT_MOD; not needed with hld -r
```

With hld the link step can be `hld -r --kernel-module -o module ...`, and the
patch step goes away (plain `hld -r` keeps HP's PROGBITS `.shtmod`).

## What HP ld -r does (measured)

Probed with small objects of known content (`tests/rel/*.s`, `*.c`), each
combined by HP ld -r and the output read back:

- **Contents are not touched.** Every output section is the plain
  concatenation of its inputs, each contribution aligned to its own
  `sh_addralign` -- checked byte for byte on gvtape's 14 sections. No
  relocation is applied, not even a call between two functions of one
  object (`PCREL21BI` against the local `vt_claim` survives).
- **Sections combine by exact name.** `.text.f` and `.text` stay apart, as
  do `.sdata.x` and `.sdata`, and gcc's per-function
  `.IA_64.unwind.text.f`. All of HP cc's per-function `.text` sections are
  named `.text` and so become one.
- **Section order** in the output: unwind tables, unwind descriptors, other
  read-only data (`.HP.opt_annot`, `.rodata`), code, writable data, short
  data, short bss, bss, then what is not loaded (`.note`, `.comment`,
  debug). Each `.rela` follows its section.
- **Relocations** keep type and addend. One against a section symbol is
  rewritten against the output section's symbol with the addend increased
  by the contribution's offset (`.text + 0` in the second function's
  unwind entry becomes `.text + 320`); one against a named symbol, local or
  global, names that symbol. Relocation sections: `sh_flags` 0,
  `sh_addralign` 4.
- **Unwind tables**: `sh_info` names the code section, `sh_link` is 0,
  `SHF_LINK_ORDER` is set and `sh_entsize` is 4 -- even when the input was
  GNU as's, which names the code in `sh_link`.
- **Symbols**: one named STT_SECTION symbol per output section (including
  `.note`, `.comment`, debug); then, per input, its FILE symbol (if it has
  one -- gcc's objects do not) and its locals; then the globals. Input
  section symbols are dropped, including HP cc's named ones
  (`$dbglline_...`). COMMON stays COMMON (`SHN_COMMON` or
  `SHN_IA_64_ANSI_COMMON` as given) with the largest size and alignment.
  Visibility is kept (`__milli_div32I` stays HIDDEN).
- **`.note`**: the inputs' notes concatenated, then a linker footprint
  appended. HP's notes use 8-byte header words (namesz, descsz, type),
  not the gABI's 4: the footprint is type 5, name `HP`, text
  `92453-07 linker ld ... B.12.42 ... Link time: <date>`. The compiler's
  are type 1 (compiler version and options) and type 4 (source, directory,
  language).
- **e_flags** -- each bit set in one input, in both, in neither:

  | bit | name | one input | both | rule |
  |---|---|---|---|---|
  | 0x1 | TRAPNIL | dropped | dropped | only `-z` sets it (`-z` -> 0x19) |
  | 0x2, 0x4 | -- | dropped | dropped | |
  | 0x20 | REDUCEDFP | **error** | kept | must match |
  | 0x40 | CONS_GP | kept | kept | or |
  | 0x80 | NOFUNCDESC_CONS_GP | **error** | kept | must match |
  | 0x100 | ABSOLUTE | kept | kept | or |
  | 0x200 .. 0x10000 | -- | dropped | dropped | |
  | 0xff000000 | architecture | kept | kept | highest (1,2 -> 2; 1,3 -> 3) |

  The error reads `Mismatched NOFUNCDESC_CONS_GP. Expected Undefined, which
  was set by the file: pa.o, but found EF_IA_64_NOFUNCDESC_CONS_GP in the
  file: pb_nfd.o.` -- the first input sets the expectation. Only the first
  mismatching bit is reported. CONS_GP is not part of the must-match test:
  0x98 + 0xd8 and 0x18 + 0x58 both link (to 0xd8 and 0x58); 0x198 + 0x158
  fails only because 0x80 differs.
- **Archives** are searched (`-l` and a named `.a` both work); **-o**
  defaults to `a.out`; a **duplicate strong global** is an error; an input
  `.shtmod` already of type SHT_MOD is **accepted and kept** (so the
  ordering advice in gvtape_shtmod.s, "patch only after the final ld -r",
  is not needed with B.12.42).
- HP ld writes the output file even when it then exits 1.

## What a loadable module contains (measured)

Census of the 36 modules in `/usr/conf/mod` on 11.23 (read-only). Every one
has e_flags **0x1b8** (BE, ABI64, REDUCEDFP, NOFUNCDESC_CONS_GP, ABSOLUTE).
20 have a `.shtmod` section -- the loadable ones (cdfs, rng, ipf, dmp*, ...);
16 do not -- linked statically into the kernel (Usb*, fclp, kconfig, osc).

| | loadable (20) | static (16) |
|---|---|---|
| `.shtmod`, type 0x60000001 (SHT_MOD), 27 bytes, DIR64MSB at offset 16 | all | none |
| `.data.KC_META`, `.data.KC_META_PTR` | all | all |
| `GPREL22` | **0** | 251 |
| calls to `__milli*` | **none** | 7 modules |
| COMMON symbols (`SHN_IA_64_ANSI_COMMON`, referenced by IMM64) | yes | yes |

Relocation types in the loadable modules, all of them: `IMM64`, `DIR64MSB`,
`SEGREL64MSB`, `SECREL32MSB`, `SECREL64MSB` (debug), `PCREL21BI`, `TPREL22`
(against kernel symbols). Never `PCREL21B`, `GPREL*`, `LTOFF*`, `FPTR*`.

What the loader itself accepts was read statically (not by loading) out of 11.23's
`/stand/vmunix` by the parallel gcc-for-modules work (its REPORT.md, same
day): `mod_obj_relone` dispatches NONE, IMM14/22/64, DIR32/64, GPREL64I,
GPREL32/64, LTOFF64I, PCREL60B, PCREL21B/BI/M/F, PCREL22, PCREL32/64,
PCREL64I, LTOFF_FPTR64I/32/64, SEGREL32/64, SECREL32/64, LTV32/64, LDXMOV,
TPREL22 and TPREL64 to handlers (the report does not split MSB from LSB;
hld's list assumes both, and LSB never occurs on this target) -- **PCREL21B to the same one as
PCREL21BI** -- and everything else, notably **GPREL22, LTOFF22, LTOFF22X,
LTOFF_FPTR22, FPTR64 and PLTOFF**, to "MOD: mod_obj_doreloc: Can't process
reloc type %d" (GPREL22's known refusal is the positive control). Millicode
(`__milli_divI`, `__milli_div32U`) is a *local* symbol in the kernel, so a
module cannot bind it ("Undefined symbol %s in loadable module %s"). The
statically linked modules use both freely because the kernel link resolves
them. Other loader diagnostics in the kernel's strings: "Reloc for
non-default symtab", "Sh_info out of range", "Cannot allocate space for
COMMON symbols" (COMMON is supported).

Two things the census also shows:

- HP's **bundled cc puts small statics in short data even in kernel mode**
  (`-Wc,-codeconvention,esnonreloc`): `static int n;` is reached with
  `GPREL22` against `.sbss` (fixture `tests/rel/tkm_sdata.c`). gvtape pads
  such variables past 8 bytes for this reason; `--kernel-module` catches
  the mistake.
- HP cc **without** the kernel option emits `LTOFF22X`/`LDXMOV` and
  `PCREL21B` (the never-loaded `kcskel` skeleton); with it, `PCREL21BI` and
  `IMM64`. gcc emits `PCREL21B` for calls: no shipped module has one, but
  the loader's dispatch treats it as PCREL21BI (static reading, not loaded).

## hld -r

`hld -r` implements the behaviour above (`src/relocatable.c`), with these
differences from HP ld -r, all deliberate. `-r` alone does what HP ld -r
does, `.shtmod` included; `--kernel-module` (hld only) is the module mode.

| | HP ld -r | hld -r | why harmless |
|---|---|---|---|
| `.shtmod` type | PROGBITS as assembled (an SHT_MOD input is kept) | the same with `-r`; **SHT_MOD (0x60000001) with `--kernel-module`** | it is the type the loader demands; `patch_shtmod.py` becomes a no-op |
| `.note` | inputs + HP linker footprint | inputs only | not loaded; hld does not fabricate HP's footprint |
| `.comment` | inputs only | inputs + hld's `what` stamp | not loaded; says which linker built it |
| relocation sections | `sh_addralign` 4, file offset 4-aligned | 8, 8-aligned | stricter alignment for 8-byte fields; the content is the same |
| non-loaded section order | HP's own (`.note` first) | first seen | order carries no meaning |
| symbol order | locals as HP; globals in HP's hash order | locals as HP; globals first seen | consumers look symbols up by name or index from the relocations |
| output on error | written, exit 1 | not written (a partial one removed), exit 1 | callers go by the exit status |

`--kernel-module` (hld only) emits `.shtmod` as SHT_MOD and refuses, after an
otherwise successful `-r`: a relocation type the loader's dispatch does not
process (list above); an undefined `__milli*`; no `.shtmod`; a `.shtmod`
without its wrapper pointer (DIR64MSB at offset 16); no `.data.KC_META` /
`.data.KC_META_PTR`. It warns for a type the dispatch accepts but no shipped
loadable module uses (GPREL64I, LTOFF64I, ... -- gp- or linkage-table-relative
in a module that has neither), and for inputs without NOFUNCDESC_CONS_GP.

### A target release: `--kernel-module=11.23` / `--kernel-module=11.31`

(hld only; proposed spelling.) Everything `--kernel-module` does, plus the
output's e_flags are that release's kernel convention regardless of the
inputs: **0x198** for 11.23 (what its HP cc kernel objects carry) and
**0x1b8** for 11.31 (`+kernel`, and every module in `/usr/conf/mod`). This is
what lets gcc objects become a module without a binary post-pass: GNU as
`-mauto-pic` writes 0x98, it cannot spell ABSOLUTE or REDUCEDFP, and HP ld
refuses 0x98 next to 11.31's 0x1b8 ("Mismatched REDUCEDFP").

The flags are asserted only where they can be backed:

- every input must already have NOFUNCDESC_CONS_GP (0x80) -- an object built
  with function descriptors and its own gp is not kernel code, and hld
  refuses it naming the file, rather than relabel it;
- the REDUCEDFP mismatch HP ld refuses is not checked between inputs;
  instead, for 11.31, the code itself is checked: every instruction that
  names a floating-point register must name only f0, f1 or f6-f11, and each
  one that does not is reported by object, section, offset and function
  (`src/kmod_fp.c`);
- ABSOLUTE is backed by the relocation check: a module with no gp-relative
  or linkage-table relocation addresses its data absolutely;
- the inputs' CONS_GP bit and anything else are not carried; the
  architecture byte is.

#### Checking the REDUCEDFP promise

What REDUCEDFP allows, measured: across the 36 modules of 11.23's
`/usr/conf/mod` (all 0x1b8) the only FRs named are f0, f1 and f6-f11 (f6
1825 times ... f11 331), and gcc keeps to the same set with
`-mfixed-range=f2-f5,f12-f127` (the parallel gcc work's ablation; without
it gcc allocates f12-f15 and f32-f61). f2-f5 are outside too.

The checker decodes only the formats that carry an FR field -- every F-unit
format, and on the M unit ldf/ldfp/stf, setf, getf and chk.s.m with an FR --
reading each field only where its format has one. It was validated against
GNU objdump's disassembly, instruction by instruction: on HP's 64-bit
`libm.so.1` both find the same 32,184 instructions naming an FR outside the
set (27,621 bundles), and the same bundles in `libc.so.1` (1,137) and
`dld.so` (230); on the 36 modules both find none. `tests/asm/fp_wide.s`
breaks the rule once in each of nine formats and must be reported nine
times, its conforming lines not at all.

### Compared with HP ld -r on real modules

`build/hld-objcmp` compares two relocatable objects by contract (sections,
contents, relocations, symbols, flags -- by name, not index). Inputs built on
11.23 by HP's recipe, linked by both linkers:

**gvtape** (`gvtape.o gvtape_wrapper.o gvtape_modmeta.o gvtape_shtmod.o`;
13 HP cc functions, KC_META, `.shtmod`, HP debug sections, 240 relocations):

| difference | class |
|---|---|
| `.shtmod`: type PROGBITS (HP) vs SHT_MOD (hld `--kernel-module`; plain `-r` matches HP) | harmless -- intended; it is what `patch_shtmod.py` makes of HP's output |
| `.note`: 0x8e0 (HP) vs 0x838 bytes | harmless -- HP's 0xa8-byte linker footprint; the inputs' notes are identical |
| `.comment`: only in hld's | harmless -- hld's version stamp, not loaded |

Nothing else differs: e_flags (0x198), all 14 other sections' type, flags,
size, alignment, entsize, links and contents; all 240 relocations (type,
offset, target, addend); all 16 globals and 15 locals. `--kernel-module`
accepts it without a warning.

**gvtape built with gcc 4.7.4 and GNU as** (the recipe being tried in
parallel: `-mno-pic -mno-sdata -Wa,--mauto-pic`, `.shtmod` in GNU syntax,
a private `kmod_memcpy`; every input e_flags 0x98, i.e. NOFUNCDESC_CONS_GP
without ABSOLUTE): the same `.shtmod` and `.note` differences, and `.comment`
differs in more than the stamp -- HP's linker clears the inputs' `SHF_MERGE |
SHF_STRINGS` and entsize 1, hld keeps them (harmless: still a run of
strings, not loaded). All relocations and symbols agree. `--kernel-module`
accepts it (56 `PCREL21B` calls). Output e_flags 0x98 from both linkers --
neither invents the ABSOLUTE bit gcc's objects lack.

**The same, with HP as's `.shtmod`** (`kmod-gcc/g3`, the parallel work's
final build; e_flags 0x98 + 0x198 -> 0x198 from both linkers, ABSOLUTE or'ed):
the `.shtmod` type with `--kernel-module`, `.note`, `.comment`; nothing else.

**kcskel** (hp-ux-driver-development's skeleton, built as its own script
does, *without* the kernel code model): the same `.note` and `.comment`
differences and nothing else. `--kernel-module` refuses it -- 8 `LTOFF22X`/
`LDXMOV` relocations -- consistent with that skeleton never having loaded.

Neither output has been loaded by a kernel; see "Open" below.

## Open

- **A real load.** The comparison proves hld -r's output says the same thing
  as HP's, which loads; it does not prove the loader agrees. The test: on a
  throwaway 11.23 guest, install hld's `gvtape` (built with
  `hld -r --kernel-module`, no patch step) in place of HP's and run the
  gvtape smoke test (`kcmodule -B gvtape=loaded`, `lsdev`, `dd`/`mt`
  through `/dev/rmt/gvt0m*`, unload). Expected differences visible to the
  loader: none besides the `.note`/`.comment` sizes.
- **`PCREL21B` in a module** (gcc's call form): absent from every shipped
  loadable module; the loader's dispatch sends it to PCREL21BI's handler
  (static reading). A gcc-built module's load test settles it.
- **Should plain `-r` emit SHT_MOD for `.shtmod` too?** hld does it only
  under `--kernel-module`, so that `-r` alone is HP ld -r. A decision for
  the project owner.
- **11.31.** Not measured: its modules are 0x1b8 too, and its `.HP.opt_annot`
  may carry an `sh_info`, which hld -r refuses rather than carries.
- **SHT_GROUP / COMDAT** inputs are refused by `-r` (C kernel code has none).
