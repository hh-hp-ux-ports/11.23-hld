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
        "  -u SYM       treat SYM as undefined (archive extraction; accepted)\n"
        "  -V           print version\n"
        "hld links static LP64 HP-UX/IA-64 executables. Shared libraries,\n"
        "archives and dynamic executables are not implemented yet.\n");
}

int main(int argc, char **argv)
{
    hld_link L;
    int i;
    int ninputs = 0;

    memset(&L, 0, sizeof L);
    L.osec_tail = &L.osecs;
    L.out_path = "a.out";
    L.entry_name = "main";      /* HP ld's default on this platform */

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (a[0] != '-' || !a[1]) {
            if (hld_add_object(&L, a) < 0) goto fail;
            ninputs++;
            continue;
        }
        if (strcmp(a, "-o") == 0 && i + 1 < argc) { L.out_path = argv[++i]; continue; }
        if (strcmp(a, "-e") == 0 && i + 1 < argc) { L.entry_name = argv[++i]; continue; }
        if (strcmp(a, "-u") == 0 && i + 1 < argc) { ++i; continue; }
        if (strcmp(a, "-z") == 0) { L.trapnil = 1; continue; }
        if (strcmp(a, "-m") == 0) { L.map = 1; continue; }
        if (strcmp(a, "-v") == 0) { L.verbose = 1; continue; }
        if (strcmp(a, "-V") == 0) {
            printf("hld — HP-UX 11.23 IA-64 LP64 linker (foundation)\n");
            return 0;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }

        /* Options accepted by HP ld that hld cannot honor yet. */
        if (strcmp(a, "-b") == 0) {
            fprintf(stderr, "hld: -b (shared library output) is not implemented yet\n");
            return 1;
        }
        if (a[0] == '-' && a[1] == 'l') {
            fprintf(stderr, "hld: %s: libraries are not implemented yet "
                            "(static objects only)\n", a);
            return 1;
        }
        if (a[0] == '-' && a[1] == 'L') { continue; }  /* harmless without -l */
        if (strcmp(a, "+Accept") == 0 && i + 1 < argc) { ++i; continue; }

        fprintf(stderr, "hld: unrecognized option `%s'\n", a);
        usage();
        return 1;
    }

    if (!ninputs) { usage(); return 1; }

    if (hld_collect_sections(&L) < 0) goto fail;
    if (hld_resolve_symbols(&L) < 0) goto fail;
    if (hld_layout(&L) < 0) goto fail;
    if (hld_build_contents(&L) < 0) goto fail;
    if (hld_relocate(&L) < 0) goto fail;
    if (hld_write_exec(&L) < 0) goto fail;
    if (L.map) hld_print_map(&L);

    hld_link_free(&L);
    return 0;

fail:
    fprintf(stderr, "hld: %s\n", L.err[0] ? L.err : "link failed");
    hld_link_free(&L);
    return 1;
}
