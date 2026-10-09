# The CLI/behavior contract hld must satisfy

## What gcc 4.7.4 actually invokes

Captured from `gcc -mlp64 -v` when linking a small dynamic executable:

```
collect2 -z +Accept TypeMismatch -u main -o hello /usr/lib/hpux64/unix98.o \
  -L<9 dirs, some nonexistent> hello.o -lgcc -lc -lunwind -lgcc
```

collect2 execs `/usr/ccs/bin/ld` with essentially this argv. Requirements extracted:

| element | required behavior |
|---|---|
| `-z` | set the TRAPNIL bit (e_flags 0x1); chatr "nulptr references" toggle |
| `+Accept TypeMismatch` | two argv tokens; relax symbol-type mismatch diagnostics |
| `-u sym` | seed undefined symbol (drives archive extraction; main comes from hello.o here) |
| `-o file` | output |
| bare `.o` args | object inputs, order-preserving |
| `-L dir` | add search dir; **tolerate nonexistent dirs**; full `-L` list is embedded as DT_RUNPATH by default |
| `-l name` | search `lib<name>.so` then `lib<name>.a` in `-L` dirs then defaults (`.so` preferred; exact preference rules TBD — HP ld has `+s`/`-a shared_archive` modes) |
| default entry | **`main`** (observed: e_entry = main's address; no `_start`, no crt0) |
| default output | dynamic executable, deferred (lazy) binding, fastbind section emitted (we may omit), RUNPATH embedded |

gcc `-shared` invokes ld with `-b` (HP flag for shared-lib output) — same library/`-L`
handling, produces ET_DYN with SONAME unset unless `+h name`.

## Flags to support early (v1 surface)

`-o -u -z -L -l -b +Accept <arg> +h <name> +b <path> -B <mode>(immediate|deferred)
-e <entry> -s(strip) -x/-X(local sym filtering) -m(map to stdout) +vnocompatwarnings
-a archive|shared|... (archive/shared preference) -N? (TBD)` — plus silently ignoring
the HP no-ops we don't need at first, **loudly** rejecting anything semantic we don't
implement (never silently mislink).

## Behavioral notes from the map/chatr

- Map output (`-m`): segment-grouped sections with linker-defined symbols listed
  inline. hld's `-m` should be information-equivalent, not byte-identical.
- chatr defaults on HP-ld output: LD_LIBRARY_PATH enabled first, SHLIB_PATH second,
  embedded RUNPATH third, deferred binding, "global hash table disabled" (but sized
  as if present), fastbind present-but-disabled, kernel-assisted branch prediction
  enabled, nulptr references enabled.
- `unix98.o` is a normal input object (provides `__xpg4_extended_mask`), nothing special.
- **Library resolution failure mode to preserve**: missing/wrong-ABI `-lc` says
  `ld: Can't find library or mismatched ABI for -lc` + `Fatal error.` — hld should keep
  ABI-checking inputs (reject ELF32/ILP32 or LSB inputs with a clear message).

## Integration path (how gcc will use hld)

gcc finds `ld` via COMPILER_PATH (`/usr/ccs/bin/` is baked in as the fallback). For
testing without touching the system: `gcc -mlp64 -B/path/to/hld-as-ld/` where that dir
contains `ld` → hld (collect2 honors -B for ld lookup), or `-Wl,` pass-throughs; later
a spec file or a symlink swap makes it permanent. **Never replace the system
`/usr/ccs/bin/ld`** on a machine relied on for other work — hld gets used via -B/specs
until it earns trust, the same policy any prudent toolchain swap would follow.

## Relocatable output (`-r`)

How an HP-UX kernel module is linked: `ld -r -o mod a.o b.o meta.o shtmod.o`.
Measured against HP ld -r (92453-07, footprint `B.12.42`) on 11.23; the
details and the evidence are in `docs/kernel-modules.md`.

| element | required behavior |
|---|---|
| `-r` | ET_REL output, every relocation kept; may appear anywhere on the line |
| `-o file` | output; **default `a.out`** as for any link (measured) |
| bare `.o` args | combined in command-line order |
| archives, `-L`/`-l` | searched as in any link: a member is pulled in for an undefined reference (measured: `ld -r pa.o libpb.a` and `-L. -lpb` both pull `pb.o`) |
| shared library input | refused -- a relocatable object has nowhere to record the dependency |
| `-z` | sets TRAPNIL (0x1) in the output; **nothing else does** -- an input's TRAPNIL is not inherited (measured) |
| `-u sym` | an undefined symbol in the output, and an archive search for it |
| e_flags | REDUCEDFP (0x20) and NOFUNCDESC_CONS_GP (0x80) must match the first input, else the link fails (`Mismatched NOFUNCDESC_CONS_GP. Expected ..., which was set by the file: A, but found ... in the file: B.`); CONS_GP (0x40) and ABSOLUTE (0x100) are or'ed; the architecture byte (0xff000000) takes the highest value; every other bit is dropped |
| duplicate strong definitions | an error (`Duplicate symbol "x" in files a.o and b.o`); same-named locals are fine |
| `-b`, `-e`, `+h`, `+b`, `+e`, `-B symbolic`, `-m` | hld refuses them with `-r`: they describe a loadable image |
| `--kernel-module` | hld only: emit `.shtmod` as SHT_MOD, and refuse what the module loader cannot load (see `docs/kernel-modules.md`) |

HP's linker writes its output even when it reports an error (exit 1). hld
writes nothing on failure; a caller checking the exit status cannot tell the
difference.
