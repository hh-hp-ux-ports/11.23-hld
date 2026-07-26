# GNU ld and ia64-hpux: it never happened

Checked directly against the binutils 2.46.1 source tree:

- `ld/configure.tgt` ia64 entries: `ia64-*-elf*`, `ia64-*-freebsd*`, `ia64-*-netbsd*`,
  `ia64-*-linux*`, `ia64-*-*vms*`, `ia64-*-aix*` (a discontinued AIX/ia64 target).
  **No `ia64-*-hpux*` pattern exists.** Configuring ld for this target fails as
  unsupported.
- `ld/emulparams/`: `elf64_ia64.sh`, `elf64_ia64_fbsd.sh`, `elf64_ia64_vms.sh` — no hpux.
- What *does* exist: BFD vectors `ia64_elf32_hpux_be_vec` / `ia64_elf64_hpux_be_vec`
  (`bfd/config.bfd` ia64*-*-hpux*) — object read/write support serving **gas** and the
  binary tools (objdump/readelf/nm/ar). That is why gcc-on-hpux has always been
  "GNU as + HP ld": the assembler side was ported (with HP-specific relocation handling
  in `bfd/elfxx-ia64.c`, e.g. page-size and short-data-section quirks, HP-UX symbol
  processing hooks), the linker side never was.
- gold: never had any ia64 port. lld/mold: no ia64.
- Even GNU ld's *ia64-linux* emulation would cover none of the HP-UX essentials: the
  dynamic-loader handshake (HP-specific dynamic tags, load map, global hash table),
  descriptor-in-.plt import model, colon-list PT_INTERP, `.IA_64.unwind_hdr`,
  branch-stub-island policy, HP program header set, MSB instruction patching. The one
  genuinely reusable part — IA-64 instruction/data field insertion
  (`bfd/elfxx-ia64.c`'s `ia64_elf_install_value`) — is a self-contained routine, and hld
  vendors it directly (`src/ia64_patch.c`, GPLv3-or-later; see COPYING) rather than
  reimplementing it independently, since it is small, well-isolated from the rest of
  BFD, and has two decades of production use behind it.

Consequence: there is no existing free linker to fix or port cheaply; a purpose-built
LP64 HP-UX linker is the honest path. What this project takes from binutils: *knowledge*
(`include/elf/ia64.h` relocation numbering, gas's ia64-hpux quirks), *tools* (cross
gas/readelf/objdump for generating and inspecting test inputs, and ia64-linux GNU ld as
a byte-level oracle for validating hld's own bundle patching, since instruction bundles
are little-endian under both ABIs), and the one vendored leaf above. The BFD/elflink
linking framework itself stays out — see `README.md` for the vendoring policy.
