/*
 * link.h — hld's internal link state: input pieces, output sections,
 * the symbol table, and the layout/emit entry points.
 */
#ifndef HLD_LINK_H
#define HLD_LINK_H

#include "elf64.h"
#include "version.h"

/* HP-UX/IPF LP64 address space (docs/format-notes.md). */
#define HLD_TEXT_BASE 0x4000000000000000ULL
#define HLD_DATA_BASE 0x6000000000000000ULL
#define HLD_SEG_ALIGN 0x10000ULL   /* file offset must be congruent to vaddr */

struct osec;

/* One input section contributing to an output section. */
typedef struct isec {
    hld_elf *obj;
    uint32_t idx;             /* section index within obj */
    const hld_shdr *sh;       /* NULL for a run of bytes hld generated */
    uint64_t out_off;         /* offset within the output section */
    struct osec *out;
    struct stubisl *island;   /* set when this is a stub island */
    struct isec *next;        /* next contribution to the same output section */
} isec;

typedef struct osec {
    const char *name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr, off, size, align, entsize;
    uint32_t shndx;           /* index in the output section header table */
    uint8_t *data;            /* materialized contents (NULL for NOBITS) */
    isec *first, **tail;      /* contributions in link order */
    struct osec *next;
} osec;

typedef struct dsosym {
    const char *name;
    uint64_t value;
    struct dsosym *next;
} dsosym;

typedef struct hld_dso {
    hld_elf *elf;
    const char *soname;       /* DT_SONAME, or the file's basename */
    dsosym *hash[1021];
    int needed;               /* something actually resolved to it */
    int indirect;             /* pulled in as another library's dependency */
    uint32_t strx;            /* soname offset in .dynstr */
    struct hld_dso *next;
} hld_dso;

typedef enum {
    HLD_SYM_UNDEF,            /* referenced, not yet defined */
    HLD_SYM_IMPORT,           /* satisfied by a shared library */
    HLD_SYM_DEFINED,          /* defined by an input section */
    HLD_SYM_ABS,              /* absolute value (linker-defined or SHN_ABS) */
    HLD_SYM_COMMON            /* tentative definition, not yet allocated */
} hld_symkind;

typedef struct hld_gsym {
    const char *name;
    hld_symkind kind;
    uint64_t value;           /* final address (ABS: the value itself) */
    uint64_t size;
    uint8_t type, bind, other;
    int short_common;         /* COMMON belongs in the short bss (.sbss) */
    isec *in;                 /* defining input section (DEFINED) */
    uint64_t in_off;          /* offset within that input section */
    hld_elf *obj;             /* defining object, for diagnostics */
    hld_dso *dso;             /* IMPORT: the library that defines it */
    uint64_t hint;            /* IMPORT: its address there, a binding hint */
    uint64_t plt_slot;        /* IMPORT: byte offset within .plt */
    uint64_t stub_off;        /* IMPORT: byte offset within the stub section */
    uint32_t dynidx;          /* index in .dynsym */
    uint32_t strx;            /* name offset in .dynstr */
    struct hld_gsym *next;    /* hash chain */
} hld_gsym;

#define HLD_SYMHASH 1021
#define HLD_LNKHASH 1021
#define HLD_ARHASH  1021

/* One member of an `ar` archive, and the archive itself. */
typedef struct arsym {
    const char *name;         /* into the member's own string table */
    size_t member;
    struct arsym *next;
} arsym;

typedef struct armember {
    const char *name;
    size_t off, len;          /* within the archive's buffer */
    int extracted;
    hld_elf *elf;             /* parsed once, borrowing that buffer */
} armember;

typedef struct hld_archive {
    char *path;
    uint8_t *data;
    size_t size;
    armember *members;
    size_t nmembers;
    arsym *hash[HLD_ARHASH];  /* our own index: defined globals -> member */
    size_t nrejected;         /* members that could not be read */
    char reject[160];         /* why the first of them could not be */
    struct hld_archive *next;
} hld_archive;

/*
 * A linkage-table entry. Both the DLT (the GOT: 8-byte slots holding an
 * address, reached gp-relatively) and the descriptor table (.opd: 16-byte
 * {code address, gp} pairs) are keyed the same way — by the target the
 * relocation names, plus an addend.
 */
/* What a DLT slot holds. */
#define HLD_DLT_PLAIN 0           /* the target's address */
#define HLD_DLT_FPTR  1           /* the address of its descriptor */
#define HLD_DLT_TPREL 2           /* its offset from the thread pointer */

typedef struct lnkent {
    struct hld_gsym *g;       /* global target, or NULL for a local one */
    struct isec *in;          /* local target's input section */
    uint64_t off;             /* addend (global) or offset within `in` */
    int kind;                 /* DLT only: what the slot holds */
    uint64_t slot;            /* byte offset within the DLT / .opd */
    struct lnkent *hnext;     /* hash chain */
    struct lnkent *next;      /* allocation order, for filling contents */
} lnkent;

/*
 * A call whose target is too far away for a direct branch goes through one
 * of these instead: a single bundle holding a wide branch, placed near the
 * caller. Stubs are grouped into islands laid into the text at intervals, so
 * that whichever call needs one has an island within a direct branch's reach.
 */
#define HLD_STUB_BUNDLE 16

typedef struct stubent {
    struct hld_gsym *g;       /* target, or NULL for a local one */
    struct isec *in;          /* local target's section */
    uint64_t off;             /* addend, or offset within `in` */
    uint64_t slot;            /* byte offset within the island */
    struct stubisl *isl;      /* the island holding it */
    struct stubent *next;
} stubent;

typedef struct stubisl {
    struct isec *at;          /* the run of bytes it occupies */
    uint64_t size;
    stubent *stubs;
    struct stubisl *next;
} stubisl;

/*
 * A data word that has to hold the address of something in another module.
 * hld cannot write it — only the loader knows where the other module lands —
 * so the word is seeded with the link-time binding and handed to the loader
 * as a dynamic relocation.
 */
typedef struct dynrel {
    struct isec *in;          /* input section holding the word */
    uint64_t off;             /* offset within that section */
    struct hld_gsym *g;       /* the imported symbol */
    uint64_t addend;          /* added to the symbol's address */
    uint32_t type;            /* R_IA64_DIR64MSB or R_IA64_FPTR64MSB */
} dynrel;

/*
 * Which kind of library -l looks for, set by HP's -a option. gcc emits it
 * around a single -l to pull that one library out of an archive while the
 * rest of the link stays shared.
 */
typedef enum {
    HLD_LIB_DEFAULT,          /* shared first, then archive */
    HLD_LIB_ARCHIVE,          /* archives only */
    HLD_LIB_SHARED,           /* shared libraries only */
    HLD_LIB_ARCHIVE_SHARED    /* archive first, then shared */
} hld_libmode;

typedef struct {
    /* inputs */
    hld_elf **objs;
    size_t nobjs, objs_cap;

    /* outputs */
    osec *osecs, **osec_tail;
    size_t nosecs;

    hld_gsym *hash[HLD_SYMHASH];

    /* linkage tables */
    lnkent *dlt_hash[HLD_LNKHASH], *dlt, **dlt_tail;
    lnkent *opd_hash[HLD_LNKHASH], *opd, **opd_tail;
    /*
     * Descriptors reached gp-relatively, for @pltoff. HP's compiler calls
     * external functions this way where gcc emits a direct branch, so an
     * object from either compiler links.
     */
    lnkent *pltoff_hash[HLD_LNKHASH], *pltoff, **pltoff_tail;
    uint64_t ndlt, nopd, npltoff;
    osec *dltsec, *opdsec, *pltoffsec;

    /* unwind tables */
    osec *unwind_hdr_sec, *unwind_sec, *unwind_info_sec;

    /* thread-local storage template */
    uint64_t tls_base, tls_filesz, tls_memsz, tls_off;
    int has_tls;

    /* layout results */
    uint64_t text_addr, text_end, text_filesz;
    uint64_t data_addr, data_off, data_filesz, data_memsz;
    uint64_t entry;
    uint64_t gp;

    hld_libmode libmode;      /* what -l looks for, per HP's -a */

    /* archives, in command-line order */
    hld_archive *archives, **ar_tail;
    size_t narchives;

    /* dynamic output */
    int dynamic;
    int shared;               /* -b: emit a shared library, not an executable */
    const char *soname;       /* +h, or the output's basename */
    uint32_t soname_strx;
    hld_dso *dsos, **dso_tail;
    size_t ndsos, nimports;
    uint64_t ndltrel;         /* DLT slots the loader has to fill */
    dynrel *dynrels;          /* data words naming another module's symbol */
    size_t ndynrel, dynrel_cap;
    stubisl *islands;         /* long-branch stubs, spread through the code */
    uint64_t nstubs, nislands;
    uint64_t nreladyn;        /* entries written to .rela.dyn so far */
    osec *pltsec, *reladynsec, *stubsec;
    char **libpaths;
    size_t nlibpaths, libpaths_cap;
    char *dynstr;
    size_t dynstr_len, dynstr_cap;
    uint32_t ndynsym, nbucket, ndyntags, nphdr;
    uint64_t reserve_off, loadmap_off;
    osec *commentsec;         /* the stamp saying which linker built this */
    osec *interpsec, *dynsymsec, *dynstrsec, *hashsec, *dynamicsec, *reservesec;

    /* options */
    const char *out_path;
    const char *entry_name;
    int trapnil;              /* -z */
    int map;                  /* -m */
    int verbose;

    char err[HLD_ERRSZ];
} hld_link;

/* link.c */
int  hld_add_object(hld_link *L, const char *path);
int  hld_input_object(hld_link *L, hld_elf *e);
int  hld_add_undefined(hld_link *L, const char *name);
int  hld_allocate_commons(hld_link *L);
int  hld_alloc_unwind(hld_link *L);
int  hld_finish_unwind(hld_link *L);
int  hld_alloc_linkage(hld_link *L);
int  hld_layout(hld_link *L);
int  hld_build_contents(hld_link *L);
int  hld_relocate(hld_link *L);
void hld_print_map(hld_link *L);
void hld_link_free(hld_link *L);
hld_gsym *hld_sym_lookup(hld_link *L, const char *name);
osec *osec_get(hld_link *L, const char *name, uint32_t type, uint64_t flags);
osec *osec_find_pub(hld_link *L, const char *name);

/* archive.c */
int  hld_archive_open(hld_link *L, const char *path, hld_archive **out);
int  hld_archive_search(hld_link *L, hld_archive *ar, int *extracted_any);
void hld_archive_free(hld_archive *ar);
int  hld_link_millicode(hld_link *L);

/* dynamic.c */
int  hld_add_dso(hld_link *L, const char *path);
int  hld_add_libpath(hld_link *L, const char *dir);
int  hld_find_library(hld_link *L, const char *name, hld_archive **ar_out);
int  hld_predefine_symbols(hld_link *L);
int  hld_bind_imports(hld_link *L);
int  hld_alloc_dynamic(hld_link *L);
int  hld_add_ident(hld_link *L);
int  hld_alloc_stubs(hld_link *L);
int  hld_dlt_needs_loader(hld_link *L, const lnkent *l);
int  hld_write_stubs(hld_link *L);
void hld_free_stubs(hld_link *L);
int  hld_branch_in_range(uint64_t from, uint64_t to);
stubent *hld_stub_find(hld_link *L, uint64_t from, hld_gsym *g, isec *in,
                       uint64_t off);
uint64_t hld_stub_addr(hld_link *L, const stubent *s);
isec *hld_isec_of(hld_link *L, hld_elf *obj, uint32_t shndx);
int  hld_reloc_target(hld_link *L, hld_elf *e, hld_sym *syms, size_t nsyms,
                      const hld_rela *r, hld_gsym **g_out, isec **in_out,
                      uint64_t *off_out, const char **name_out);
int  hld_fill_dynamic(hld_link *L);

/* write.c */
int  hld_write_exec(hld_link *L);

#endif /* HLD_LINK_H */
