/*
 * hld.c — driver.
 *
 * Command line follows HP ld's (docs/link-contract.md) so that gcc/collect2
 * can invoke hld unmodified. Anything not implemented is rejected with a
 * clear message rather than ignored: a linker that silently drops an option
 * it does not understand produces a subtly wrong binary, which is far worse
 * than failing.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"

#include <time.h>

/*
 * Phase timing, off unless HLD_TIME is set in the environment. A link that
 * takes minutes rather than seconds is a question about which phase, and
 * without this the only way to answer it is to guess.
 */
static int hld_time_on;
static clock_t hld_time_last;
static clock_t hld_start;

static void phase(const char *name)
{
    clock_t now;

    if (!hld_time_on) return;
    now = clock();
    if (name)
        fprintf(stderr, "hld: %-22s %6.2fs\n", name,
                (double)(now - hld_time_last) / CLOCKS_PER_SEC);
    hld_time_last = now;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: hld [options] file.o...\n"
        "  -o FILE      output file (default a.out)\n"
        "  -e SYM       entry point (default main)\n"
        "  -z           trap NULL dereferences (sets the TRAPNIL flag)\n"
        "  -m           print a link map to stdout\n"
        "  -dynamic     produce a dynamic executable\n"
        "  -u SYM       treat SYM as undefined, so archives are searched for it\n"
        "  -L DIR       add a library search directory\n"
        "  -l NAME      link libNAME.so, libNAME.so.1 or libNAME.a\n"
        "  -a MODE      what -l looks for: archive, shared, archive_shared,\n"
        "               shared_archive or default\n"
        "  -b, -shared  produce a shared library (ET_DYN) instead of a program\n"
        "  +h NAME      the name it records for itself (DT_SONAME); -soname too\n"
        "  +b DIRS      where the loader should search at run time (DT_RUNPATH);\n"
        "               the -L list is recorded too unless +nodefaultrpath\n"
        "  --start-group ... --end-group   re-search these archives until\n"
        "               nothing further is pulled in\n"
        "  -V, --version  print version and licence\n"
        "Archives are searched at their position on the command line.\n");
}

int main(int argc, char **argv)
{
    hld_link L;
    int i;
    int ninputs = 0;
    /* archives named inside --start-group ... --end-group */
    hld_archive **group = NULL;
    size_t ngroup = 0, group_cap = 0;
    int in_group = 0;

    hld_start = clock();
    memset(&L, 0, sizeof L);
    L.osec_tail = &L.osecs;
    L.out_path = "a.out";
    L.entry_name = "main";      /* HP ld's default on this platform */

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        /*
         * HP's linker spells some options with a leading '+', so they have to
         * be recognised before a bare word is taken for a file name.
         */
        if (a[0] == '+') {
            /* two-token forms */
            if (strcmp(a, "+h") == 0) {
                if (i + 1 >= argc) goto need_arg;
                L.soname = argv[++i];       /* the name recorded in DT_SONAME */
                continue;
            }
            /*
             * +b is how the compiler driver passes -rpath here. Ignoring it
             * would leave the image searching only the -L defaults, which is
             * not what the caller asked for.
             */
            if (strcmp(a, "+b") == 0) {
                if (i + 1 >= argc) goto need_arg;
                if (hld_add_rpath(&L, argv[++i]) < 0) goto fail;
                continue;
            }
            if (strcmp(a, "+nodefaultrpath") == 0) {
                L.no_runpath = 1;    /* do not record the -L list in the image */
                continue;
            }
            if (strcmp(a, "+Accept") == 0 || strcmp(a, "+e") == 0) {
                if (i + 1 < argc) ++i;
                continue;
            }
            /* diagnostic-only switches: harmless to accept and ignore */
            if (strncmp(a, "+v", 2) == 0 || strcmp(a, "+w") == 0
                || strcmp(a, "+noenvvar") == 0 || strcmp(a, "+compat") == 0) {
                continue;
            }
            fprintf(stderr, "hld: option `%s' is not implemented\n", a);
            return 1;
        need_arg:
            /*
             * Reached only for options we DO implement. Saying "not
             * implemented" here would send the caller looking for a missing
             * feature instead of a truncated command line.
             */
            fprintf(stderr, "hld: option `%s' needs an argument\n", a);
            return 1;
        }
        if (a[0] != '-' || !a[1]) {
            if (hld_add_object(&L, a) < 0) goto fail;
            ninputs++;
            continue;
        }
        if (strcmp(a, "-o") == 0 && i + 1 < argc) { L.out_path = argv[++i]; continue; }
        if (strcmp(a, "-e") == 0 && i + 1 < argc) { L.entry_name = argv[++i]; continue; }
        if (strcmp(a, "-u") == 0 && i + 1 < argc) {
            /* seed an undefined symbol so archives are searched for it */
            if (hld_add_undefined(&L, argv[++i]) < 0) goto fail;
            continue;
        }
        if (strcmp(a, "-z") == 0) { L.trapnil = 1; continue; }
        if (strcmp(a, "-m") == 0) { L.map = 1; continue; }
        if (strcmp(a, "-dynamic") == 0) { L.dynamic = 1; continue; }
        if (strcmp(a, "-v") == 0) { L.verbose = 1; continue; }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            /*
             * The first line is exactly what it has always been -- anything
             * parsing -V for a version keeps working -- with the licence
             * block after it, as the GNU tools do.
             */
            printf("%s\n", HLD_IDENT + 4);   /* past the `what` marker */
            printf("%s\n", HLD_COPYRIGHT);
            return 0;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }

        /*
         * -b is the platform spelling; the compiler driver's specs turn
         * -shared into it before the linker is reached. Accept the GNU
         * spelling too, for anything that drives ld directly.
         */
        if (strcmp(a, "-b") == 0 || strcmp(a, "-shared") == 0) {
            /* A shared library has no entry point and needs no interpreter. */
            L.shared = 1;
            L.dynamic = 1;
            L.entry_name = NULL;
            continue;
        }
        if (strcmp(a, "--start-group") == 0 || strcmp(a, "-(") == 0) {
            in_group = 1;
            ngroup = 0;
            continue;
        }
        if (strcmp(a, "--end-group") == 0 || strcmp(a, "-)") == 0) {
            /*
             * Re-search the archives in the group until a whole pass pulls
             * nothing: that is what lets two archives satisfy each other
             * regardless of the order they were named in.
             */
            int changed;
            size_t k;
            in_group = 0;
            do {
                changed = 0;
                for (k = 0; k < ngroup; k++) {
                    int any = 0;
                    if (hld_archive_search(&L, group[k], &any) < 0) goto fail;
                    if (any) changed = 1;
                }
            } while (changed);
            ngroup = 0;
            continue;
        }
        /*
         * -a chooses what -l looks for from here on. gcc brackets a single
         * library with -aarchive_shared ... -adefault for -static-libstdc++,
         * so honouring it is what makes a statically linked C++ runtime come
         * out static.
         */
        if (a[0] == '-' && a[1] == 'a' && a[2]) {
            const char *m = a + 2;
            if (strcmp(m, "archive") == 0)              L.libmode = HLD_LIB_ARCHIVE;
            else if (strcmp(m, "shared") == 0)          L.libmode = HLD_LIB_SHARED;
            else if (strcmp(m, "archive_shared") == 0)  L.libmode = HLD_LIB_ARCHIVE_SHARED;
            else if (strcmp(m, "shared_archive") == 0
                     || strcmp(m, "default") == 0)      L.libmode = HLD_LIB_DEFAULT;
            else {
                fprintf(stderr, "hld: unknown library mode `-a%s'\n", m);
                return 1;
            }
            continue;
        }
        if (strcmp(a, "-soname") == 0 && i + 1 < argc) {
            L.soname = argv[++i];           /* the GNU spelling of +h */
            continue;
        }
        if (strncmp(a, "-soname=", 8) == 0) { L.soname = a + 8; continue; }
        if (a[0] == '-' && a[1] == 'l') {
            const char *nm = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
            hld_archive *ar = NULL;
            if (!nm) { fprintf(stderr, "hld: -l needs a name\n"); return 1; }
            if (hld_find_library(&L, nm, &ar) < 0) goto fail;
            if (in_group && ar) {
                if (ngroup == group_cap) {
                    size_t nc = group_cap ? group_cap * 2 : 8;
                    hld_archive **ng = realloc(group, nc * sizeof *ng);
                    if (!ng) { fprintf(stderr, "hld: out of memory\n"); return 1; }
                    group = ng;
                    group_cap = nc;
                }
                group[ngroup++] = ar;
            }
            ninputs++;
            continue;
        }
        if (a[0] == '-' && a[1] == 'L') {
            const char *dir = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
            if (dir) hld_add_libpath(&L, dir);
            continue;
        }

        fprintf(stderr, "hld: unrecognized option `%s'\n", a);
        usage();
        return 1;
    }

    if (!ninputs) { usage(); return 1; }

    hld_time_on = getenv("HLD_TIME") != NULL;
    if (hld_time_on)
        fprintf(stderr, "hld: %-22s %6.2fs\n", "inputs (objects+libs)",
                (double)(clock() - hld_start) / CLOCKS_PER_SEC);
    phase(NULL);
    if (hld_add_ident(&L) < 0) goto fail;
    if (hld_link_millicode(&L) < 0) goto fail;
    phase("millicode");
    if (hld_allocate_commons(&L) < 0) goto fail;
    phase("commons");
    /*
     * The linker's own symbols are established before any library is
     * consulted. They name this module's layout -- `_end', `__gp', `_etext'
     * -- so a definition of them must never be taken from a shared library
     * that happens to export the same name: that binds the program to
     * another module's addresses, and the descriptor reserved for the import
     * is left unwritten once layout defines the symbol locally after all.
     */
    if (hld_predefine_symbols(&L) < 0) goto fail;
    phase("predefine");
    if (hld_bind_imports(&L) < 0) goto fail;
    phase("bind-imports");
    if (hld_alloc_unwind(&L) < 0) goto fail;
    phase("alloc-unwind");
    if (hld_alloc_linkage(&L) < 0) goto fail;
    phase("alloc-linkage");
    if (hld_alloc_dynamic(&L) < 0) goto fail;
    phase("alloc-dynamic");
    if (hld_layout(&L) < 0) goto fail;
    phase("layout");
    if (hld_alloc_stubs(&L) < 0) goto fail;
    phase("alloc-stubs");
    if (hld_time_on)
        fprintf(stderr, "hld: %-22s %6llu\n", "  long-branch stubs",
                (unsigned long long)L.nstubs);
    if (hld_build_contents(&L) < 0) goto fail;
    phase("build-contents");
    if (hld_fill_dynamic(&L) < 0) goto fail;
    phase("fill-dynamic");
    if (hld_write_stubs(&L) < 0) goto fail;
    phase("write-stubs");
    if (hld_relocate(&L) < 0) goto fail;
    phase("relocate");
    if (hld_finish_unwind(&L) < 0) goto fail;
    phase("finish-unwind");
    if (hld_write_exec(&L) < 0) goto fail;
    phase("write-exec");
    if (L.map) hld_print_map(&L);

    free(group);
    hld_link_free(&L);
    return 0;

fail:
    free(group);
    fprintf(stderr, "hld: %s\n", L.err[0] ? L.err : "link failed");
    hld_link_free(&L);
    return 1;
}
