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
        "  --start-group ... --end-group   re-search these archives until\n"
        "               nothing further is pulled in\n"
        "  -V           print version\n"
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
            if (strcmp(a, "+h") == 0 && i + 1 < argc) {
                L.soname = argv[++i];       /* the name recorded in DT_SONAME */
                continue;
            }
            if (strcmp(a, "+Accept") == 0 || strcmp(a, "+b") == 0
                || strcmp(a, "+e") == 0
                || strcmp(a, "+nodefaultrpath") == 0) {
                if (strcmp(a, "+nodefaultrpath") != 0 && i + 1 < argc) ++i;
                continue;
            }
            /* diagnostic-only switches: harmless to accept and ignore */
            if (strncmp(a, "+v", 2) == 0 || strcmp(a, "+w") == 0
                || strcmp(a, "+noenvvar") == 0 || strcmp(a, "+compat") == 0) {
                continue;
            }
            fprintf(stderr, "hld: option `%s' is not implemented\n", a);
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
        if (strcmp(a, "-V") == 0) {
            printf("%s\n", HLD_IDENT + 4);   /* past the `what` marker */
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

    if (hld_add_ident(&L) < 0) goto fail;
    if (hld_link_millicode(&L) < 0) goto fail;
    if (hld_allocate_commons(&L) < 0) goto fail;
    if (hld_bind_imports(&L) < 0) goto fail;
    if (hld_predefine_symbols(&L) < 0) goto fail;
    if (hld_alloc_unwind(&L) < 0) goto fail;
    if (hld_alloc_linkage(&L) < 0) goto fail;
    if (hld_alloc_dynamic(&L) < 0) goto fail;
    if (hld_layout(&L) < 0) goto fail;
    if (hld_alloc_stubs(&L) < 0) goto fail;
    if (hld_build_contents(&L) < 0) goto fail;
    if (hld_fill_dynamic(&L) < 0) goto fail;
    if (hld_write_stubs(&L) < 0) goto fail;
    if (hld_relocate(&L) < 0) goto fail;
    if (hld_finish_unwind(&L) < 0) goto fail;
    if (hld_write_exec(&L) < 0) goto fail;
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
