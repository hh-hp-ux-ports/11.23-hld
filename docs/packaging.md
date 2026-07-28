# Packaging and installing hld

`make package` builds an SD depot. It has to run on HP-UX, since `swpackage`
lives there, but it works as an ordinary user — no root needed to *build* one.

```
make CC="gcc -mlp64"
make package                    # -> build/hld-<version>-ia64-11.23.depot
```

The result is a serial depot file: one product, `HLD`, in three filesets.

| fileset | contents |
|---|---|
| `HLD.RUN` | `hld` and `hld-readelf` in `/opt/hld/bin`, plus the licence. Adds `/opt/hld/bin` to `/etc/PATH`. |
| `HLD.LDOVR` | An `ld` symlink in `/opt/hld/override`, and `/opt/hld/override` placed **ahead of `/usr/ccs/bin`** on `/etc/PATH`, so `ld` resolves to hld. Requires `HLD.RUN`. |
| `HLD.SRC` | The complete corresponding source under `/opt/hld/src`. |

Installing needs root:

```
swinstall -s /full/path/to/hld-<version>-ia64-11.23.depot HLD        # incl. the ld override
swinstall -s /full/path/to/hld-<version>-ia64-11.23.depot HLD.RUN    # linker only
```

The source path must be absolute, and the depot should be on local disk —
installing from an NFS path hangs.

## What the ld override does and does not cover

The override works by putting `/opt/hld/override` before `/usr/ccs/bin` on the
system PATH, so anything that resolves `ld` through PATH — a shell, a
handwritten makefile — gets hld.

**It does not redirect gcc.** gcc finds its linker through `COMPILER_PATH`,
which has `/usr/ccs/bin` compiled in, so it keeps using the system linker
regardless of PATH. To point gcc at hld:

```
gcc -mlp64 -B/opt/hld/override/ ...
```

That is also the safer way to try hld on a real build, because it applies to
one invocation instead of the whole machine.

Nothing is overwritten either way: the system linker stays exactly where it
is, and `/etc/PATH` is backed up (`/etc/PATH.pre-hld`,
`/etc/PATH.pre-hld-override`) before being edited. Removing the fileset undoes
the PATH change:

```
swremove HLD.LDOVR      # stop overriding ld, keep hld installed
swremove HLD            # remove everything
```

⚠️ hld is not yet a complete replacement for the system linker — it cannot
link archives or produce shared libraries, among other gaps listed in the
README. Installing `HLD.LDOVR` on a machine that builds other software will
break those builds. Prefer `HLD.RUN` plus `-B` until hld covers what you need.

## Source, and the licence

hld is GPLv3-or-later and includes code derived from GNU binutils, so the
binaries must not travel without the source they were built from. `HLD.SRC`
puts the whole tree in `/opt/hld/src`, which satisfies section 6(a) — the
depot carries object code and corresponding source together. It is the real
tree, so it builds:

```
cd /opt/hld/src && make CC="gcc -mlp64"
```

The HP system binaries used as test fixtures (`samples/sys/`) are deliberately
excluded: they are not part of hld's source and are not ours to redistribute.
