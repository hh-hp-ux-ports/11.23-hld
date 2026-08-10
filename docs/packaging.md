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
| `HLD.SRC` | The complete corresponding source under `/opt/hld/src`. |

Installing needs root, and the depot has to be uncompressed first — SD reads a
plain serial depot and reports a gzipped one as "doesn't look like a tar":

```
/usr/contrib/bin/gzip -dc hld-<version>-ia64-11.23.depot.gz > /var/tmp/hld.depot
swinstall -s /var/tmp/hld.depot HLD.RUN    # the linker
swinstall -s /var/tmp/hld.depot HLD        # linker and source
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

## Pointing a build at hld

gcc finds its linker through `COMPILER_PATH`, which has `/usr/ccs/bin` compiled
in, so PATH alone does not redirect it. Name the directory instead:

```
gcc -mlp64 -B<dir>/ ...        # <dir> holds  ld -> /opt/hld/bin/hld
```

That applies to one invocation rather than the whole machine, which is what you
want while hld and the system linker both have work to do. A compiler
configured `--with-ld=/opt/hld/bin/hld` uses it unconditionally, and then `-B`
cannot override it — check with `-Wl,-V`, which prints the version of the
linker that actually ran.

Nothing is overwritten: the system linker stays where it is, and `/etc/PATH` is
backed up (`/etc/PATH.pre-hld`) before `HLD.RUN` appends to it.

```
swremove HLD            # remove everything
```

⚠️ **There was a third fileset, `HLD.LDOVR`, which put an `ld` ahead of
`/usr/ccs/bin` on `/etc/PATH`. It was removed in 0.12.1.** It never worked —
`ld` is also reachable as `/usr/bin/ld`, a symlink to the same HP linker, and
`/usr/bin` comes earlier on `/etc/PATH`, so the fileset installed, reported
`configured`, and changed nothing. Making it work would be worse than leaving
it out: hld links LP64 only and refuses ELF32, so a bare `ld` resolving to hld
breaks every 32-bit link on a machine that needs both. Use `-B` or `--with-ld`.

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
