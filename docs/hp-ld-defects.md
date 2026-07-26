# HP ld B.12.34 defect dossier → hld regression targets

The linker being replaced: `/usr/ccs/bin/ld` = `92453-07 linker ld HP Itanium(R) B.12.34
IPF/IPF, REL Thu Feb 23 2006` (file dated 2006-03-15, 10 MB, itself built with aC++
A.06.02). The linker+fdp cumulative patch (PHSS_34353) is installed; it is the newest
linker patch obtainable for this OS release — later patches existed only in vendor
support archives that are no longer reachable. All three defects below were root-caused
empirically against real HP-UX 11.23/Itanium hardware during GNU toolchain porting work;
the key evidence (before/after disassembly, fault analysis) is reproduced inline below.

## Defect 1 — R_IA64_GPREL64I corrupts slot 0 of the MLX bundle

gcc ≥ 4.9 (PR60465) addresses local symbols (= every string literal) with
`movl reg = @gprel(sym)` → **R_IA64_GPREL64I** against an MLX bundle. HP ld mis-applies
it: **slot 0 is overwritten along with the L+X immediate slots**. Observed by comparing
the same bundle before and after linking:

```
before linking (correct): [MLX]  mov r35=r1                  <- GP save (slot 0)
                                 movl r36=0x0                <- GPREL64I target (L+X)
after HP ld (corrupt):     [MLX]  cmp.lt p0,p0=r0,r0         <- slot 0 DESTROYED
                                 movl r36=0x42a014000000000  <- garbage immediate
```

Every program touching a string literal SIGILLs. The known compiler-side workaround
is to force the older `@ltoff` addressing sequence whenever the target linker is HP's —
it costs one extra load per local-symbol reference, permanently.

**hld acceptance**: assemble an MLX `movl @gprel` object matching this shape (a modern
gcc emits it directly), link with hld, verify by disassembly that slot 0 is untouched
and the L+X immediate is the correct gp-relative value; then confirm a toolchain built
without the compiler-side workaround links and runs correctly through hld.

## Defect 2 — out-of-range direct br.call instead of a long-branch stub

In a sufficiently large compiler binary (text size exceeding PCREL21B's ±16 MB reach),
HP ld emitted for one call a direct `br.call` to a bogus address several MB past the end
of text — while correctly emitting long-branch stub entries for neighboring calls. The
callee resolved to a global object (a function descriptor) in the data segment, so a
PC-relative direct branch to it is nonsense in any reading. The bad target is
deterministic per binary but changes on relink. This is the **currently blocking**
defect: a modern gcc's C++ compiler binary is large enough to trigger it and cannot be
linked correctly by HP ld at all, with no available workaround — it is the immediate
motivation for this project.

**hld acceptance**: branch-target distance checking is *structural* — every PCREL21B
whose target is out of range or not text gets a stub island (or brl rewrite), verified
by an exhaustive post-link scan of all branch targets (a checker tool worth writing
regardless: `hld-verify` walking every B-unit branch in the output). End goal: link a
real large compiler binary that exercises this path and confirm it runs correctly.

## Defect 3 — unwind r_offset handling disagreement

A `.rela.IA_64.unwind` r_offset slot-encoding disagreement between GNU gas output and
HP ld (the encoding question — which slot a given r_offset low-bit pattern designates —
was never formally settled against the psABI). This affected an earlier binutils
component and has surfaced through unwind-table handling more than once.

**hld acceptance**: a C++ exception-handling end-to-end test (assemble → link →
throw/catch runs correctly), with unwind table entries validated sorted and
segment-relative by `hld-verify`.

## Why not other routes

- A newer HP linker release: no longer obtainable from any available channel; the
  installed patch level is the ceiling. (A linker binary from a later OS release would
  be a licensing and compatibility gamble even if found.)
- GNU ld: no ia64-hpux emulation exists (`docs/gnu-ld-status.md`); porting GNU ld's
  elfxx-ia64 backend to the HP-UX ABI (dynamic-loader handshake, DLT model, HP dynamic
  tags, unwind_hdr, fastbind...) is comparable work to a fresh linker with none of the
  BFD baggage, and ia64 support upstream is maintenance-orphaned.
- gold/lld/mold: no ia64 port ever existed.
