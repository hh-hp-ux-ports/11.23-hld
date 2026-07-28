# Packaging and installing hld

`make package` builds an SD depot. It has to run on HP-UX, since `swpackage`
lives there, but it works as an ordinary user — no root needed to *build* one.

```
make CC="gcc -mlp64"
make package                    # -> build/hld-<version>-ia64-11.23.depot.gz
```

The result is a gzipped serial depot: one product, `HLD`, in three filesets.
It is built with

```
swpackage -s build/hld.psf -x target_type=tape -d <path>.depot
```

and then gzipped, which takes it from about 1.3 MB to 280 KB.

| fileset | contents |
|---|---|
| `HLD.RUN` | `hld` and `hld-readelf` in `/opt/hld/bin`, plus the licence. Adds `/opt/hld/bin` to `/etc/PATH`. |
| `HLD.LDOVR` | An `ld` symlink in `/opt/hld/override`, and `/opt/hld/override` placed **ahead of `/usr/ccs/bin`** on `/etc/PATH`, so `ld` resolves to hld. Requires `HLD.RUN`. |
| `HLD.SRC` | The complete corresponding source under `/opt/hld/src`. |

Installing needs root, and the depot has to be uncompressed first — SD reads a
plain serial depot and reports a gzipped one as "doesn't look like a tar":

```
/usr/contrib/bin/gzip -dc hld-<version>-ia64-11.23.depot.gz > /var/tmp/hld.depot
swinstall -s /var/tmp/hld.depot HLD        # incl. the ld override
swinstall -s /var/tmp/hld.depot HLD.RUN    # linker only
```

The source path must be absolute, and the depot should be on local disk —
installing from an NFS path hangs.

### Why the depot is gzipped rather than compressed by SD

SD can store a depot's files compressed, with `swinstall` decompressing them
on the way in, which would be neater. It is not usable here, for three
reasons established by trying it:

- `-x compress_files=true` is refused for `target_type=tape`, and a serial
  depot *is* the tape type. It is also refused with `reinstall_files=false`,
  which is swpackage's default.
- Copying the serial depot into a compressed directory depot does not work
  either: `swcopy` reports that "file compression may not be used when
  copying from tape".
- Building the directory depot directly needs root — non-root `swpackage -d
  <dir>` fails with "Cannot create depot ... You are not authorized".

So SD-native compression needs root and a directory depot. Gzipping the serial
depot is the usual way HP-UX depots are handed around anyway, costs nothing to
undo, and keeps the whole thing buildable as an ordinary user.

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
produce shared libraries, among other gaps listed in the README. Installing
`HLD.LDOVR` on a machine that builds other software will break those builds.
Prefer `HLD.RUN` plus `-B` until hld covers what you need.

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
