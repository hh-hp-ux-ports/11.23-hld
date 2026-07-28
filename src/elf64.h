/*
 * elf64.h — ELF64 definitions for the HP-UX 11.23 / IPF LP64 world, plus the
 * parsed in-memory forms hld uses. Target files are big-endian (ELFDATA2MSB);
 * all access to file bytes goes through the beNN/stNN accessors below — never
 * struct overlay.
 *
 * Constants are from the public gABI/psABI numbering and from observation of
 * real HP-UX 11.23/Itanium binaries (see docs/format-notes.md). Anything provisional is
 * marked PROVISIONAL.
 */
#ifndef HLD_ELF64_H
#define HLD_ELF64_H

#include <stddef.h>
#if defined(__hpux) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L)
/*
 * The vendor compiler's C89 mode leaves <stdint.h> without the fixed-width
 * types; <sys/types.h> has them (with _HPUX_SOURCE, which port.h sets).
 */
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/*
 * `inline` is C99. The vendor compiler on the target predates it, and these
 * accessors are small enough that plain `static` costs nothing there.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define HLD_INLINE static inline
#else
#define HLD_INLINE static
#endif

/* ---- byte accessors (file data is MSB) --------------------------------- */

HLD_INLINE uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
HLD_INLINE uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
HLD_INLINE uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}
HLD_INLINE void st16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
HLD_INLINE void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
HLD_INLINE void st64(uint8_t *p, uint64_t v)
{
    st32(p, (uint32_t)(v >> 32)); st32(p + 4, (uint32_t)v);
}

/* Instruction bundles are little-endian regardless of ELF data encoding. */
HLD_INLINE uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
HLD_INLINE void stle64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

/* ---- e_ident ----------------------------------------------------------- */

#define EI_NIDENT    16
#define ELFMAG       "\177ELF"
#define ELFCLASS64   2
#define ELFDATA2MSB  2
#define EV_CURRENT   1
#define ELFOSABI_HPUX 1
/* Observed on every sample: EI_ABIVERSION = 1. */
#define HPUX_ABIVERSION 1

/* e_type */
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3

#define EM_IA_64 50

/* e_flags */
#define EF_IA_64_TRAPNIL 0x00000001  /* -z: trap NIL derefs (obs: HP ld outputs) */
#define EF_IA_64_BE      0x00000008
#define EF_IA_64_ABI64   0x00000010

/* On-disk record sizes (ELF64) */
#define EHDR64_SIZE 64
#define PHDR64_SIZE 56
#define SHDR64_SIZE 64
#define SYM64_SIZE  24
#define RELA64_SIZE 24
#define DYN64_SIZE  16

/* ---- section types / flags -------------------------------------------- */

#define SHT_NULL          0
#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_STRTAB        3
#define SHT_RELA          4
#define SHT_HASH          5
#define SHT_DYNAMIC       6
#define SHT_NOTE          7
#define SHT_NOBITS        8
#define SHT_REL           9
#define SHT_SHLIB         10
#define SHT_DYNSYM        11
#define SHT_INIT_ARRAY    14
#define SHT_FINI_ARRAY    15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP         17
#define SHT_SYMTAB_SHNDX  18
#define SHT_LOOS          0x60000000u
#define SHT_HP_OPT_ANNOT  0x60000004u /* .HP.opt_annot in HP cc objects (obs) */
#define SHT_HIOS          0x6fffffffu
#define SHT_IA_64_EXT     0x70000000u
#define SHT_IA_64_UNWIND  0x70000001u

#define SHF_WRITE            0x1u
#define SHF_ALLOC            0x2u
#define SHF_EXECINSTR        0x4u
#define SHF_MERGE            0x10u
#define SHF_STRINGS          0x20u
#define SHF_INFO_LINK        0x40u
#define SHF_LINK_ORDER       0x80u
#define SHF_OS_NONCONFORMING 0x100u
#define SHF_GROUP            0x200u
#define SHF_TLS              0x400u
#define SHF_IA_64_SHORT      0x10000000u /* gp-addressed: .sdata/.sbss/.dlt/.plt */

/* special section indexes */
#define SHN_UNDEF  0
#define SHN_LORESERVE 0xff00u
/*
 * IA-64 "ANSI C common": like SHN_COMMON, but such a symbol takes precedence
 * over a weak definition, and it belongs in the short-addressable bss. gcc
 * emits it for ordinary tentative definitions (`int x;` at file scope).
 */
#define SHN_IA_64_ANSI_COMMON SHN_LORESERVE
#define SHN_ABS    0xfff1u
#define SHN_COMMON 0xfff2u
#define SHN_XINDEX 0xffffu

/* ---- program header types ---------------------------------------------- */

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7
#define PT_LOOS    0x60000000u
#define PT_HP_TLS              (PT_LOOS + 0x0)
#define PT_HP_CORE_NONE        (PT_LOOS + 0x1)
#define PT_HP_CORE_VERSION     (PT_LOOS + 0x2)
#define PT_HP_CORE_KERNEL      (PT_LOOS + 0x3)
#define PT_HP_CORE_COMM        (PT_LOOS + 0x4)
#define PT_HP_CORE_PROC        (PT_LOOS + 0x5)
#define PT_HP_CORE_LOADABLE    (PT_LOOS + 0x6)
#define PT_HP_CORE_STACK       (PT_LOOS + 0x7)
#define PT_HP_CORE_SHM         (PT_LOOS + 0x8)
#define PT_HP_CORE_MMF         (PT_LOOS + 0x9)
#define PT_HP_PARALLEL         (PT_LOOS + 0x10)
#define PT_HP_FASTBIND         (PT_LOOS + 0x11)
#define PT_HP_OPT_ANNOT        (PT_LOOS + 0x12)
#define PT_HP_HSL_ANNOT        (PT_LOOS + 0x13)
#define PT_HP_STACK            (PT_LOOS + 0x14)
#define PT_HP_CORE_UTSNAME     (PT_LOOS + 0x15)
#define PT_HP_LINKER_FOOTPRINT (PT_LOOS + 0x16) /* obs: unmapped .note */
#define PT_IA_64_ARCHEXT 0x70000000u
#define PT_IA_64_UNWIND  0x70000001u

#define PF_X 0x1u
#define PF_W 0x2u
#define PF_R 0x4u

/*
 * HP-specific segment flags. The dynamic loader refuses an image ("not a
 * valid load module") whose loadable segments do not carry them, so they
 * are not advisory: text needs PF_HP_CODE, data needs PF_HP_MODIFY.
 * Bit 0x20000 is unnamed in the public headers but is set by the platform's
 * linker on text; it is reproduced here for the same reason.
 */
#define PF_HP_CODE      0x00040000u
#define PF_HP_MODIFY    0x00080000u
#define PF_HP_PAGE_SIZE 0x00100000u
#define PF_HP_LAZYSWAP  0x00800000u
#define PF_HP_UNNAMED17 0x00020000u

/* ---- dynamic tags ------------------------------------------------------ */

#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3   /* holds __gp on this platform (obs) */
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_SONAME       14
#define DT_RPATH        15
#define DT_SYMBOLIC     16
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define DT_PLTREL       20
#define DT_DEBUG        21
#define DT_TEXTREL      22
#define DT_JMPREL       23
#define DT_BIND_NOW     24
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH      29  /* HP ld embeds the -L list here by default (obs) */
#define DT_FLAGS        30
#define DT_PREINIT_ARRAY   32
#define DT_PREINIT_ARRAYSZ 33

#define DF_STATIC_TLS 0x10

/* HP-UX OS range (canonical names per binutils elf/hppa.h; values observed) */
#define DT_HP_LOAD_MAP        0x60000000
#define DT_HP_DLD_FLAGS       0x60000001
#define DT_HP_DLD_HOOK        0x60000002
#define DT_HP_UX10_INIT       0x60000003
#define DT_HP_UX10_INITSZ     0x60000004
#define DT_HP_PREINIT         0x60000005
#define DT_HP_PREINITSZ       0x60000006
#define DT_HP_NEEDED          0x60000007
#define DT_HP_TIME_STAMP      0x60000008
#define DT_HP_CHECKSUM        0x60000009
#define DT_HP_GST_SIZE        0x6000000a
#define DT_HP_GST_VERSION     0x6000000b
#define DT_HP_GST_HASHVAL     0x6000000c
#define DT_HP_EPLTREL         0x6000000d
#define DT_HP_EPLTRELSZ       0x6000000e
#define DT_HP_FILTERED        0x6000000f
#define DT_HP_FILTER_TLS      0x60000010
#define DT_HP_COMPAT_FILTERED 0x60000011
#define DT_HP_LAZYLOAD        0x60000012
#define DT_HP_BIND_NOW_COUNT  0x60000013
#define DT_IA_64_PLT_RESERVE  0x70000000

/* DT_HP_DLD_FLAGS bits (binutils elf/hppa.h) */
#define DT_HP_DF_DEBUG_PRIVATE   0x00001
#define DT_HP_DF_DEBUG_CALLBACK  0x00002
#define DT_HP_DF_CALLBACK_BOR    0x00004
#define DT_HP_DF_NO_ENVVAR       0x00008
#define DT_HP_DF_BIND_NOW        0x00010
#define DT_HP_DF_BIND_NONFATAL   0x00020
#define DT_HP_DF_BIND_VERBOSE    0x00040
#define DT_HP_DF_BIND_RESTRICTED 0x00080
#define DT_HP_DF_BIND_SYMBOLIC   0x00100
#define DT_HP_DF_RPATH_FIRST     0x00200
#define DT_HP_DF_BIND_DEPTH_FIRST 0x00400
#define DT_HP_DF_GST             0x00800
#define DT_HP_DF_SHLIB_FIXED     0x01000
#define DT_HP_DF_MERGE_SHLIB_SEG 0x02000
#define DT_HP_DF_NODELETE        0x04000
#define DT_HP_DF_GROUP           0x08000
#define DT_HP_DF_PROTECT_LT      0x10000

/* ---- symbols ----------------------------------------------------------- */

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4
#define STT_COMMON  5
#define STT_TLS     6
#define STV_DEFAULT   0
#define STV_INTERNAL  1
#define STV_HIDDEN    2
#define STV_PROTECTED 3

#define ELF64_ST_BIND(i)   ((uint8_t)(i) >> 4)
#define ELF64_ST_TYPE(i)   ((uint8_t)(i) & 0xf)
#define ELF64_ST_INFO(b,t) ((uint8_t)(((b) << 4) | ((t) & 0xf)))
#define ELF64_ST_VISIBILITY(o) ((uint8_t)(o) & 0x3)

#define ELF64_R_SYM(i)    ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)   ((uint32_t)((i) & 0xffffffffu))
#define ELF64_R_INFO(s,t) (((uint64_t)(s) << 32) | (uint64_t)(t))

/* ---- IA-64 relocation types (psABI numbering; *MSB variants apply here) --
 * X-macro so the dumper and the linker share one table. */
#define HLD_IA64_RELOC_LIST(X) \
    X(R_IA64_NONE,            0x00) \
    X(R_IA64_IMM14,           0x21) \
    X(R_IA64_IMM22,           0x22) \
    X(R_IA64_IMM64,           0x23) \
    X(R_IA64_DIR32MSB,        0x24) \
    X(R_IA64_DIR32LSB,        0x25) \
    X(R_IA64_DIR64MSB,        0x26) \
    X(R_IA64_DIR64LSB,        0x27) \
    X(R_IA64_GPREL22,         0x2a) \
    X(R_IA64_GPREL64I,        0x2b) \
    X(R_IA64_GPREL32MSB,      0x2c) \
    X(R_IA64_GPREL32LSB,      0x2d) \
    X(R_IA64_GPREL64MSB,      0x2e) \
    X(R_IA64_GPREL64LSB,      0x2f) \
    X(R_IA64_LTOFF22,         0x32) \
    X(R_IA64_LTOFF64I,        0x33) \
    X(R_IA64_PLTOFF22,        0x3a) \
    X(R_IA64_PLTOFF64I,       0x3b) \
    X(R_IA64_PLTOFF64MSB,     0x3e) \
    X(R_IA64_PLTOFF64LSB,     0x3f) \
    X(R_IA64_FPTR64I,         0x43) \
    X(R_IA64_FPTR32MSB,       0x44) \
    X(R_IA64_FPTR32LSB,       0x45) \
    X(R_IA64_FPTR64MSB,       0x46) \
    X(R_IA64_FPTR64LSB,       0x47) \
    X(R_IA64_PCREL60B,        0x48) \
    X(R_IA64_PCREL21B,        0x49) \
    X(R_IA64_PCREL21M,        0x4a) \
    X(R_IA64_PCREL21F,        0x4b) \
    X(R_IA64_PCREL32MSB,      0x4c) \
    X(R_IA64_PCREL32LSB,      0x4d) \
    X(R_IA64_PCREL64MSB,      0x4e) \
    X(R_IA64_PCREL64LSB,      0x4f) \
    X(R_IA64_LTOFF_FPTR22,    0x52) \
    X(R_IA64_LTOFF_FPTR64I,   0x53) \
    X(R_IA64_LTOFF_FPTR32MSB, 0x54) \
    X(R_IA64_LTOFF_FPTR32LSB, 0x55) \
    X(R_IA64_LTOFF_FPTR64MSB, 0x56) \
    X(R_IA64_LTOFF_FPTR64LSB, 0x57) \
    X(R_IA64_SEGREL32MSB,     0x5c) \
    X(R_IA64_SEGREL32LSB,     0x5d) \
    X(R_IA64_SEGREL64MSB,     0x5e) \
    X(R_IA64_SEGREL64LSB,     0x5f) \
    X(R_IA64_SECREL32MSB,     0x64) \
    X(R_IA64_SECREL32LSB,     0x65) \
    X(R_IA64_SECREL64MSB,     0x66) \
    X(R_IA64_SECREL64LSB,     0x67) \
    X(R_IA64_REL32MSB,        0x6c) \
    X(R_IA64_REL32LSB,        0x6d) \
    X(R_IA64_REL64MSB,        0x6e) \
    X(R_IA64_REL64LSB,        0x6f) \
    X(R_IA64_LTV32MSB,        0x74) \
    X(R_IA64_LTV32LSB,        0x75) \
    X(R_IA64_LTV64MSB,        0x76) \
    X(R_IA64_LTV64LSB,        0x77) \
    X(R_IA64_PCREL21BI,       0x79) \
    X(R_IA64_PCREL22,         0x7a) \
    X(R_IA64_PCREL64I,        0x7b) \
    X(R_IA64_IPLTMSB,         0x80) \
    X(R_IA64_IPLTLSB,         0x81) \
    X(R_IA64_EPLTMSB,         0x82) /* PROVISIONAL: HP export-PLT (obs. in libc, absent from GNU headers; pairs with DT_HP_EPLTREL) */ \
    X(R_IA64_EPLTLSB,         0x83) /* PROVISIONAL: LSB twin by pattern */ \
    X(R_IA64_COPY,            0x84) \
    X(R_IA64_SUB,             0x85) \
    X(R_IA64_LTOFF22X,        0x86) \
    X(R_IA64_LDXMOV,          0x87) \
    X(R_IA64_TPREL14,         0x91) \
    X(R_IA64_TPREL22,         0x92) \
    X(R_IA64_TPREL64I,        0x93) \
    X(R_IA64_TPREL64MSB,      0x96) \
    X(R_IA64_TPREL64LSB,      0x97) \
    X(R_IA64_LTOFF_TPREL22,   0x9a) \
    X(R_IA64_DTPMOD64MSB,     0xa6) \
    X(R_IA64_DTPMOD64LSB,     0xa7) \
    X(R_IA64_LTOFF_DTPMOD22,  0xaa) \
    X(R_IA64_DTPREL14,        0xb1) \
    X(R_IA64_DTPREL22,        0xb2) \
    X(R_IA64_DTPREL64I,       0xb3) \
    X(R_IA64_DTPREL32MSB,     0xb4) \
    X(R_IA64_DTPREL32LSB,     0xb5) \
    X(R_IA64_DTPREL64MSB,     0xb6) \
    X(R_IA64_DTPREL64LSB,     0xb7) \
    X(R_IA64_LTOFF_DTPREL22,  0xba)

enum {
#define X(n, v) n = v,
    HLD_IA64_RELOC_LIST(X)
#undef X
    R_IA64__dummy_end = 0xfff
};

/* ---- parsed in-memory forms ------------------------------------------- */

typedef struct {
    uint8_t  cls, data, osabi, abiver;
    uint16_t type;
    uint16_t machine;
    uint32_t flags;
    uint64_t entry, phoff, shoff;
    uint16_t phentsize, phnum, shentsize, shstrndx;
    uint32_t shnum;
} hld_ehdr;

typedef struct {
    uint32_t name_off;
    const char *name;        /* into shstrtab, or "" */
    uint32_t type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} hld_shdr;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} hld_phdr;

typedef struct {
    uint32_t name_off;
    const char *name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} hld_sym;

typedef struct {
    uint64_t offset;
    uint32_t sym, type;
    int64_t addend;
} hld_rela;

typedef struct {
    int64_t tag;
    uint64_t val;
} hld_dyn;

typedef struct {
    char *path;
    uint8_t *data;
    size_t size;
    int owns_data;            /* 0 when the buffer belongs to someone else */
    hld_ehdr eh;
    hld_shdr *shdrs;         /* eh.shnum entries (0 if none) */
    hld_phdr *phdrs;         /* eh.phnum entries */
} hld_elf;

/* elfread.c */
#define HLD_ERRSZ 256
hld_elf *hld_elf_load(const char *path, char *err /* HLD_ERRSZ */);
hld_elf *hld_elf_from_memory(const char *name, uint8_t *data, size_t size,
                             int owns_data, char *err);
void hld_elf_free(hld_elf *e);
/* Bounds-checked pointer to a section's file bytes (NULL for NOBITS/oob). */
const uint8_t *hld_sec_data(const hld_elf *e, const hld_shdr *sh, char *err);
/* Read a SYMTAB/DYNSYM section into a malloc'd array; names resolved. */
hld_sym *hld_read_syms(const hld_elf *e, const hld_shdr *sh, size_t *count, char *err);
/* Read a RELA section into a malloc'd array. */
hld_rela *hld_read_relas(const hld_elf *e, const hld_shdr *sh, size_t *count, char *err);
/* Read a DYNAMIC section into a malloc'd array (terminating DT_NULL kept). */
hld_dyn *hld_read_dyn(const hld_elf *e, const hld_shdr *sh, size_t *count, char *err);
/* String from a STRTAB section by offset ("" + err on oob). */
const char *hld_strtab_str(const hld_elf *e, uint32_t strtab_ndx, uint64_t off);
/* Locate the section containing vaddr (alloc sections only), or NULL. */
const hld_shdr *hld_sec_by_addr(const hld_elf *e, uint64_t addr);
/* Name helpers (dump + diagnostics). */
const char *hld_reloc_name(uint32_t type);      /* NULL if unknown */
const char *hld_sht_name(uint32_t type);        /* NULL if unknown */
const char *hld_pt_name(uint32_t type);         /* NULL if unknown */
const char *hld_dt_name(int64_t tag);           /* NULL if unknown */

#endif /* HLD_ELF64_H */
