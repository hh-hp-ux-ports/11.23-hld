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
