/*
 * relocatable.c -- relocatable output (-r): several objects combined into
 * one, every relocation kept for whoever links or loads the result next.
 *
 * This is how an HP-UX kernel module is made: the compiler's objects, the
 * KC_META records and the .shtmod record go through `ld -r', and the
 * kernel's module loader applies the relocations itself when the module is
 * loaded. So nothing here may be resolved early. The rules below are what
 * HP ld -r does, measured against it on 11.23 (docs/kernel-modules.md);
 * where hld departs from it, the comment at that spot says so and why.
 *
 *  - Sections are combined by exact name, in input order, each contribution
 *    aligned as its input asked. Contents are copied unchanged: HP ld -r
 *    patches nothing, not even a call between two functions of one object.
 *  - Every relocation is kept, with its type and addend. One against a
 *    section symbol is moved to the output section's symbol and its addend
 *    grows by where the contribution landed; one against any other symbol
 *    names that symbol's new index.
 *  - Local symbols are kept, per input, behind one named section symbol per
 *    output section. Globals are resolved: two strong definitions are an
 *    error, a tentative definition (COMMON) stays tentative.
 *  - An unwind table names the code it describes in sh_info, sh_link 0 and
 *    SHF_LINK_ORDER set, entsize 4 -- the platform's spelling, whichever
 *    spelling (HP's or GNU's) the input used.
 *  - e_flags: see rel_flags().
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "kmod_fp.h"

static void rerr(hld_link *L, const char *fmt, const char *a, const char *b)
{
    snprintf(L->err, HLD_ERRSZ, fmt, a ? a : "", b ? b : "");
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (a < 2) ? v : ((v + a - 1) & ~(a - 1));
}

/* ---- inputs ------------------------------------------------------------ */

/*
 * How the code-model bits of e_flags combine, measured bit by bit against
 * HP ld -r (each bit set in one input, in both, in neither):
 *
 *   REDUCEDFP (0x20), NOFUNCDESC_CONS_GP (0x80)
 *        must agree with the first input, or the link fails -- the objects
 *        were compiled for different code models. A kernel object built with
 *        -Wc,-codeconvention,esnonreloc (11.31: +kernel) has 0x80 set; an
 *        ordinary one does not, and they cannot be mixed.
 *   CONS_GP (0x40), ABSOLUTE (0x100)
 *        or'ed, silently.
 *   architecture byte (0xff000000)
 *        the highest value wins.
 *   TRAPNIL (0x1)
 *        not inherited: set only by -z, as for any other output.
 *   anything else
 *        dropped.
 */
static int rel_flags(hld_link *L, hld_elf *e)
{
    static const struct { uint32_t bit; const char *name; } must[] = {
        { EF_IA_64_NOFUNCDESC_CONS_GP, "NOFUNCDESC_CONS_GP" },
        { EF_IA_64_REDUCEDFP,          "REDUCEDFP" },
    };
    uint32_t f = e->eh.flags;
    size_t i;

    /*
     * With a target release named, the output's code-model bits are that
     * release's kernel convention, not a combination of the inputs' -- the
     * gcc route to a module, whose assembler cannot spell ABSOLUTE or
     * REDUCEDFP, depends on it. What cannot be asserted for an input is
     * the code model itself: an object compiled with function descriptors
     * and a gp of its own is not kernel code whatever its header says.
     */
    if (L->kmod_target && !(f & EF_IA_64_NOFUNCDESC_CONS_GP)) {
        snprintf(L->err, HLD_ERRSZ,
                 "%s: e_flags 0x%x lack NOFUNCDESC_CONS_GP (0x80): not "
                 "compiled for the kernel (HP cc -Wc,-codeconvention,"
                 "esnonreloc or +kernel; gcc -mno-pic -Wa,--mauto-pic)",
                 e->path, (unsigned)f);
        return -1;
    }
    if (!L->rflags_from) {
        L->rflags = f & (EF_IA_64_REDUCEDFP | EF_IA_64_CONS_GP
                         | EF_IA_64_NOFUNCDESC_CONS_GP | EF_IA_64_ABSOLUTE
                         | EF_IA_64_ARCH);
        L->rflags_from = e->path;
        return 0;
    }
    for (i = 0; i < sizeof must / sizeof must[0]; i++) {
        /* REDUCEDFP is the target's to set, and is checked in the code */
        if (L->kmod_target && must[i].bit == EF_IA_64_REDUCEDFP) continue;
        if ((f & must[i].bit) != (L->rflags & must[i].bit)) {
            snprintf(L->err, HLD_ERRSZ,
                     "mismatched %s: %s has it %s, but %s, the first input, "
                     "has it %s -- objects compiled for different code models "
                     "(a kernel object and an ordinary one?) cannot be combined",
                     must[i].name, e->path,
                     (f & must[i].bit) ? "set" : "clear", L->rflags_from,
                     (L->rflags & must[i].bit) ? "set" : "clear");
            return -1;
        }
    }
    L->rflags |= f & (EF_IA_64_CONS_GP | EF_IA_64_ABSOLUTE);
    if ((f & EF_IA_64_ARCH) > (L->rflags & EF_IA_64_ARCH))
        L->rflags = (L->rflags & ~EF_IA_64_ARCH) | (f & EF_IA_64_ARCH);
    return 0;
}

/*
 * The output's e_flags. A named target release gets its kernel convention,
 * measured on that release's own modules and compilers: 11.23's HP cc
 * kernel objects are 0x198 (BE, ABI64, NOFUNCDESC_CONS_GP, ABSOLUTE);
 * 11.31's +kernel objects and every module in either release's
 * /usr/conf/mod are 0x1b8, adding REDUCEDFP.
 */
static uint32_t rel_out_flags(const hld_link *L)
{
    uint32_t f = EF_IA_64_BE | EF_IA_64_ABI64
               | (L->trapnil ? EF_IA_64_TRAPNIL : 0);

    if (!L->kmod_target) return f | L->rflags;
    f |= EF_IA_64_NOFUNCDESC_CONS_GP | EF_IA_64_ABSOLUTE
       | (L->rflags & EF_IA_64_ARCH);
    if (L->kmod_target == 1131) f |= EF_IA_64_REDUCEDFP;
    return f;
}

static int is_shtmod(const char *name)
{
    return strcmp(name, ".shtmod") == 0;
}

/*
 * Two sections of the same name are one output section; that only works if
 * they are the same kind of section. NOBITS and PROGBITS may meet (the
 * output becomes PROGBITS), and .shtmod may arrive as either PROGBITS (all
 * the assembler can make) or the type it has to end up with.
 */
static int types_compatible(const char *name, uint32_t a, uint32_t b)
{
    if (a == b) return 1;
    if ((a == SHT_NOBITS && b == SHT_PROGBITS)
        || (a == SHT_PROGBITS && b == SHT_NOBITS)) return 1;
    if (is_shtmod(name)
        && (a == SHT_PROGBITS || a == SHT_HP_MOD)
        && (b == SHT_PROGBITS || b == SHT_HP_MOD)) return 1;
    return 0;
}

static int rel_sections(hld_link *L, hld_elf *e)
{
    uint32_t j;
    int nsymtab = 0;

    if (!e->isec_by_shndx && e->eh.shnum) {
        e->isec_by_shndx = calloc(e->eh.shnum, sizeof *e->isec_by_shndx);
        if (!e->isec_by_shndx) { rerr(L, "out of memory", NULL, NULL); return -1; }
    }

    for (j = 1; j < e->eh.shnum; j++) {
        hld_shdr *sh = &e->shdrs[j];
        osec *o;
        isec *in;
        char what[32];

        switch (sh->type) {
        case SHT_NULL:
        case SHT_STRTAB:
        case SHT_RELA:
            continue;               /* rebuilt for the output */
        case SHT_SYMTAB:
            if (++nsymtab > 1) {
                rerr(L, "%s: more than one symbol table", e->path, NULL);
                return -1;
            }
            continue;
        case SHT_REL:
        case SHT_GROUP:
        case SHT_SYMTAB_SHNDX:
        case SHT_DYNAMIC:
        case SHT_DYNSYM:
        case SHT_HASH:
            /*
             * None of these occurs in an object the platform's compilers
             * make for C. Each would need handling of its own -- a group is
             * a set of sections to keep or drop together -- and dropping or
             * copying it blindly would produce an object that is wrong in a
             * way nothing reports. So refuse, naming it.
             */
            snprintf(what, sizeof what, "%s", hld_sht_name(sh->type));
            snprintf(L->err, HLD_ERRSZ, "%s: section `%s' is of type %s, "
                     "which hld -r does not handle", e->path, sh->name, what);
            return -1;
        default:
            break;
        }

        o = osec_find_pub(L, sh->name);
        if (o && !types_compatible(sh->name, o->type, sh->type)) {
            snprintf(L->err, HLD_ERRSZ, "%s: section `%s' has type 0x%x here "
                     "but 0x%x in an earlier input, so the two cannot be "
                     "combined", e->path, sh->name, (unsigned)sh->type,
                     (unsigned)o->type);
            return -1;
        }
        if (!o) {
            o = osec_get(L, sh->name, sh->type, sh->flags);
            if (!o) { rerr(L, "out of memory", NULL, NULL); return -1; }
            o->order = (int)L->nosecs;
        } else {
            o = osec_get(L, sh->name, sh->type, sh->flags);
        }
        if (sh->addralign > o->align) o->align = sh->addralign;
        if (sh->entsize && !o->entsize) o->entsize = sh->entsize;

        in = calloc(1, sizeof *in);
        if (!in) { rerr(L, "out of memory", NULL, NULL); return -1; }
        in->obj = e;
        in->idx = j;
        in->sh = sh;
        in->out = o;
        e->isec_by_shndx[j] = in;
        in->out_off = align_up(o->size, sh->addralign ? sh->addralign : 1);
        o->size = in->out_off + sh->size;
        *o->tail = in;
        o->tail = &in->next;
    }

    /*
     * Second pass, now that every section of this object has its record:
     * what each one says it belongs to. An unwind table describes one code
     * section -- HP's assembler names it in sh_info, GNU's in sh_link -- and
     * the output has to name the OUTPUT section that code went to.
     */
    for (j = 1; j < e->eh.shnum; j++) {
        hld_shdr *sh = &e->shdrs[j];
        isec *in = (isec *)e->isec_by_shndx[j];
        uint32_t tgt;
        isec *tin;

        if (!in) continue;
        if (sh->type == SHT_IA_64_UNWIND) {
            tgt = sh->info ? sh->info : sh->link;
            tin = tgt < e->eh.shnum ? (isec *)e->isec_by_shndx[tgt] : NULL;
            if (!tin) {
                rerr(L, "%s: unwind section `%s' does not say which code it "
                        "describes", e->path, sh->name);
                return -1;
            }
            if (in->out->assoc && in->out->assoc != tin->out) {
                snprintf(L->err, HLD_ERRSZ, "%s: unwind section `%s' "
                         "describes `%s', but an earlier input's describes "
                         "`%s'; one output table cannot do both", e->path,
                         sh->name, tin->out->name, in->out->assoc->name);
                return -1;
            }
            in->out->assoc = tin->out;
            continue;
        }
        if (sh->link || sh->info) {
            /*
             * No other section in a C object refers to another one this
             * way. If one ever does, its reference would go stale in the
             * output -- the failure that makes GNU objcopy produce
             * unreadable files from HP's objects -- so stop here instead.
             */
            snprintf(L->err, HLD_ERRSZ, "%s: section `%s' refers to another "
                     "section (sh_link %u, sh_info %u), which hld -r cannot "
                     "carry over yet", e->path, sh->name, (unsigned)sh->link,
                     (unsigned)sh->info);
            return -1;
        }
    }
    return 0;
}

static void rel_list(hld_link *L, hld_gsym *g)
{
    if (g->r_listed) return;
    if (!L->rsyms_tail) L->rsyms_tail = &L->rsyms;
    g->r_listed = 1;
    *L->rsyms_tail = g;
    L->rsyms_tail = &g->r_next;
}

/*
 * Global symbols. Locals are not interned: they belong to their object and
 * are copied, renumbered, when the output is written.
 */
static int rel_symbols(hld_link *L, hld_elf *e)
{
    uint32_t j;

    for (j = 1; j < e->eh.shnum; j++) {
        hld_sym *syms;
        size_t n, k;
        char err[HLD_ERRSZ];

        if (e->shdrs[j].type != SHT_SYMTAB) continue;
        syms = hld_read_syms(e, &e->shdrs[j], &n, err);
        if (!syms) { snprintf(L->err, HLD_ERRSZ, "%s", err); return -1; }

        for (k = 1; k < n; k++) {
            hld_sym *s = &syms[k];
            uint8_t bind = ELF64_ST_BIND(s->info);
            uint8_t type = ELF64_ST_TYPE(s->info);
            hld_gsym *g;

            if (bind == STB_LOCAL) continue;
            if (!s->name[0]) continue;
            g = hld_sym_intern(L, s->name);
            if (!g) { rerr(L, "out of memory", NULL, NULL); free(syms); return -1; }
            rel_list(L, g);            /* fixes the output order */

            if (s->shndx == SHN_UNDEF) {
                /*
                 * Still a reference: it says what it expects. A strong
                 * reference anywhere makes the output's reference strong.
                 */
                if (g->kind == HLD_SYM_UNDEF) {
                    if (!g->bind || bind == STB_GLOBAL) g->bind = bind;
                    if (!g->type) g->type = type;
                    if (!g->other) g->other = s->other;
                }
                continue;
            }
            if (s->shndx == SHN_COMMON || s->shndx == SHN_IA_64_ANSI_COMMON) {
                /*
                 * A tentative definition stays tentative in a relocatable
                 * output -- whoever links it last allocates it. Measured:
                 * HP ld -r keeps the largest size and the largest
                 * alignment of the copies it was given.
                 */
                if (g->kind == HLD_SYM_UNDEF || g->kind == HLD_SYM_COMMON) {
                    if (g->kind == HLD_SYM_UNDEF || s->size > g->size) {
                        g->size = s->size;
                        g->short_common = (s->shndx == SHN_IA_64_ANSI_COMMON);
                    }
                    if (s->value > g->align) g->align = s->value;
                    g->kind = HLD_SYM_COMMON;
                    g->type = type;
                    g->bind = bind;
                    g->other = s->other;
                    g->obj = e;
                }
                continue;
            }
            if (s->shndx >= SHN_LORESERVE && s->shndx != SHN_ABS) {
                char idx[16];
                snprintf(idx, sizeof idx, "0x%x", (unsigned)s->shndx);
                snprintf(L->err, HLD_ERRSZ, "%s: symbol `%s' has special "
                         "section index %s, which hld -r does not know",
                         e->path, s->name, idx);
                free(syms);
                return -1;
            }
            /* a definition */
            if (g->kind == HLD_SYM_DEFINED || g->kind == HLD_SYM_ABS) {
                if (bind == STB_WEAK) continue;       /* keep the first strong */
                if (g->bind != STB_WEAK) {
                    snprintf(L->err, HLD_ERRSZ, "duplicate definition of `%s' "
                             "in %s and %s", g->name,
                             g->obj ? g->obj->path : "?", e->path);
                    free(syms);
                    return -1;
                }
            }
            if (s->shndx == SHN_ABS) {
                g->kind = HLD_SYM_ABS;
                g->value = s->value;
                g->in = NULL;
            } else {
                isec *in = hld_isec_of(L, e, s->shndx);
                if (!in) {
                    rerr(L, "%s: symbol `%s' is defined in a section that is "
                            "not carried into the output", e->path, s->name);
                    free(syms);
                    return -1;
                }
                g->kind = HLD_SYM_DEFINED;
                g->in = in;
                g->in_off = s->value;
            }
            g->size = s->size;
            g->type = type;
            g->bind = bind;
            g->other = s->other;
            g->obj = e;
        }
        free(syms);
    }
    return 0;
}

int hld_rel_input(hld_link *L, hld_elf *e)
{
    /*
     * The kernel is LP64 and so are its modules. An ILP32 relocatable link
     * is possible in principle, but nothing on this platform needs one.
     */
    if (HLD_IS32(e)) {
        rerr(L, "%s: an ELF32 (ILP32) object; hld -r combines LP64 objects "
                "only", e->path, NULL);
        return -1;
    }
    if (rel_flags(L, e) < 0) return -1;
    if (rel_sections(L, e) < 0) return -1;
    return rel_symbols(L, e);
}

/* ---- output ------------------------------------------------------------ */

/*
 * Output section order, as HP ld -r arranges it: the unwind tables, then
 * their descriptors, the rest of the read-only data, code, writable data,
 * short data, short bss, bss, and the sections that are not loaded. Within
 * a class, the order the inputs first named them. Nothing depends on the
 * order; following the platform's keeps a comparison of the two readable.
 */
static int rel_class(const osec *o)
{
    if (!(o->flags & SHF_ALLOC)) return 8;
    if (o->type == SHT_IA_64_UNWIND) return 0;
    if (strncmp(o->name, ".IA_64.unwind_info", 18) == 0) return 1;
    if (o->flags & SHF_EXECINSTR) return 3;
    if (!(o->flags & SHF_WRITE)) return 2;
    if (o->type != SHT_NOBITS)
        return (o->flags & SHF_IA_64_SHORT) ? 5 : 4;
    return (o->flags & SHF_IA_64_SHORT) ? 6 : 7;
}

static int rel_order_cmp(const void *a, const void *b)
{
    const osec *x = *(const osec *const *)a, *y = *(const osec *const *)b;
    int cx = rel_class(x), cy = rel_class(y);

    if (cx != cy) return cx < cy ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order ? 1 : 0;
}

typedef struct {
    char *buf;
    size_t len, cap;
} rstr;

static uint32_t rstr_add(hld_link *L, rstr *s, const char *str)
{
    size_t n = strlen(str) + 1;
    uint32_t off;

    if (!s->buf) {
        s->cap = 256;
        s->buf = calloc(1, s->cap);
        s->len = 1;
        if (!s->buf) { rerr(L, "out of memory", NULL, NULL); return 0; }
    }
    if (!str[0]) return 0;
    if (s->len + n > s->cap) {
        size_t nc = s->cap * 2;
        char *nb;
        while (nc < s->len + n) nc *= 2;
        nb = realloc(s->buf, nc);
        if (!nb) { rerr(L, "out of memory", NULL, NULL); return 0; }
        s->buf = nb;
        s->cap = nc;
    }
    off = (uint32_t)s->len;
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    return off;
}

/* What each input symbol became: an output index, plus an addend shift. */
typedef struct {
    uint32_t *idx;
    uint64_t *adj;
    size_t n;
    uint32_t symtab;          /* the input's symbol table section, 0 if none */
    /*
     * Section -> the first relocation section applying to it, and from each
     * relocation section to the next for the same target (normally none).
     * Looked up once per contribution; a sweep there would be quadratic in
     * the number of sections, and a C++ object has thousands.
     */
    uint32_t *relfor, *relnext;
} symmap;

/*
 * The relocation types a loadable module may carry.
 *
 * kmod_accepted: the types the 11.23 module loader's relocation dispatch
 * (mod_obj_relone in /stand/vmunix) sends to a handler; anything else takes
 * its error path, "MOD: mod_obj_doreloc: Can't process reloc type %d".
 * Read statically from the kernel by the parallel gcc-for-modules work
 * (2026-10-09), with GPREL22's known refusal as the positive control. Note
 * that GPREL22, LTOFF22, LTOFF22X, LTOFF_FPTR22, FPTR64 and PLTOFF are NOT
 * accepted -- exactly what a compiler emits for short data, the linkage
 * table and function descriptors, none of which a module has.
 *
 * kmod_seen: every type found in the 20 loadable modules (those with a
 * .shtmod) under /usr/conf/mod on 11.23. A type the dispatch accepts but no
 * shipped module uses is let through with a warning; PCREL21B, gcc's call,
 * is not warned about: the dispatch sends it to the same handler as HP's
 * PCREL21BI.
 */
static int kmod_accepted(uint32_t t)
{
    switch (t) {
    case R_IA64_NONE:
    case R_IA64_IMM14: case R_IA64_IMM22: case R_IA64_IMM64:
    case R_IA64_DIR32MSB: case R_IA64_DIR32LSB:
    case R_IA64_DIR64MSB: case R_IA64_DIR64LSB:
    case R_IA64_GPREL64I:
    case R_IA64_GPREL32MSB: case R_IA64_GPREL32LSB:
    case R_IA64_GPREL64MSB: case R_IA64_GPREL64LSB:
    case R_IA64_LTOFF64I:
    case R_IA64_PCREL60B: case R_IA64_PCREL21B:
    case R_IA64_PCREL21M: case R_IA64_PCREL21F:
    case R_IA64_PCREL32MSB: case R_IA64_PCREL32LSB:
    case R_IA64_PCREL64MSB: case R_IA64_PCREL64LSB:
    case R_IA64_LTOFF_FPTR64I:
    case R_IA64_LTOFF_FPTR32MSB: case R_IA64_LTOFF_FPTR32LSB:
    case R_IA64_LTOFF_FPTR64MSB: case R_IA64_LTOFF_FPTR64LSB:
    case R_IA64_SEGREL32MSB: case R_IA64_SEGREL32LSB:
    case R_IA64_SEGREL64MSB: case R_IA64_SEGREL64LSB:
    case R_IA64_SECREL32MSB: case R_IA64_SECREL32LSB:
    case R_IA64_SECREL64MSB: case R_IA64_SECREL64LSB:
    case R_IA64_LTV32MSB: case R_IA64_LTV32LSB:
    case R_IA64_LTV64MSB: case R_IA64_LTV64LSB:
    case R_IA64_PCREL21BI: case R_IA64_PCREL22: case R_IA64_PCREL64I:
    case R_IA64_LDXMOV:
    case R_IA64_TPREL22: case R_IA64_TPREL64MSB: case R_IA64_TPREL64LSB:
        return 1;
    default:
        return 0;
    }
}

static int kmod_seen(uint32_t t)
{
    switch (t) {
    case R_IA64_IMM64:
    case R_IA64_DIR64MSB:
    case R_IA64_SEGREL64MSB:
    case R_IA64_SECREL32MSB:
    case R_IA64_SECREL64MSB:
    case R_IA64_PCREL21BI:
    case R_IA64_PCREL21B:       /* same handler as PCREL21BI, see above */
    case R_IA64_TPREL22:
        return 1;
    default:
        return 0;
    }
}

/*
 * The function an offset in an input section belongs to, for a diagnostic
 * a person can act on: "in vt_claim+0x40" rather than ".text+0x1e2".
 */
static const char *func_at(hld_elf *e, uint32_t shndx, uint64_t off,
                           uint64_t *delta, char *buf, size_t bufsz)
{
    uint32_t j;
    const char *best = NULL;
    uint64_t bestv = 0;

    for (j = 1; j < e->eh.shnum; j++) {
        hld_sym *syms;
        size_t n, k;
        char err[HLD_ERRSZ];

        if (e->shdrs[j].type != SHT_SYMTAB) continue;
        syms = hld_read_syms(e, &e->shdrs[j], &n, err);
        if (!syms) return NULL;
        for (k = 1; k < n; k++) {
            if (syms[k].shndx != shndx
                || ELF64_ST_TYPE(syms[k].info) != STT_FUNC) continue;
            if (syms[k].value <= off && (!best || syms[k].value > bestv)
                && (!syms[k].size || off < syms[k].value + syms[k].size)) {
                best = syms[k].name;
                bestv = syms[k].value;
            }
        }
        if (best) snprintf(buf, bufsz, "%s", best);
        free(syms);
        break;
    }
    *delta = off - bestv;
    return best ? buf : NULL;
}

typedef struct {
    unsigned long refused, milli, unseen;
    char first_unseen[160];
} kmod_tally;

/*
 * One relocation, seen as the module loader will see it. What it cannot
 * process is named in full -- object, section, offset, function, symbol --
 * because the alternative is "Relocation in module gvtape failed" on the
 * console of the machine it was loaded into, naming none of them.
 */
static void kmod_note(kmod_tally *k, const isec *in, const hld_rela *r,
                      const char *sym, int64_t addend)
{
    const char *tn = hld_reloc_name(r->type);
    char tbuf[24], fn[128], where[200];
    uint64_t at = r->offset, delta = 0;
    const char *f;
    int milli = strncmp(sym, "__milli", 7) == 0;
    int refused = !kmod_accepted(r->type);

    if (!tn) { snprintf(tbuf, sizeof tbuf, "type %u", (unsigned)r->type); tn = tbuf; }
    if (!milli && !refused) {
        if (!kmod_seen(r->type) && !k->unseen++)
            snprintf(k->first_unseen, sizeof k->first_unseen,
                     "%s in %s against `%s'", tn, in->sh->name, sym);
        return;
    }
    if (refused) k->refused++;
    else k->milli++;
    if (k->refused + k->milli > 25) return;
    f = func_at(in->obj, in->idx, at & ~3ULL, &delta, fn, sizeof fn);
    if (f)
        snprintf(where, sizeof where, "%s+0x%llx (%s+0x%llx)", in->sh->name,
                 (unsigned long long)at, f,
                 (unsigned long long)(delta + (at & 3)));
    else
        snprintf(where, sizeof where, "%s+0x%llx", in->sh->name,
                 (unsigned long long)at);
    if (refused)
        fprintf(stderr, "hld: %s: %s: %s against `%s'%+lld -- the module "
                "loader does not process this type\n", in->obj->path, where,
                tn, sym, (long long)addend);
    else
        fprintf(stderr, "hld: %s: %s: call to millicode `%s' -- the kernel "
                "does not export it to modules\n", in->obj->path, where, sym);
}

/*
 * 11.31's kernel objects claim REDUCEDFP: their code names no floating-
 * point register but f0, f1 and f6-f11, so the kernel does not save the
 * rest around them. When the target release has hld make that claim, the
 * code has to keep it -- gcc does only when told (-mfixed-range=f2-f5,
 * f12-f127), and a module that breaks it corrupts other processes'
 * floating point long after it ran. Every offending instruction is named.
 */
static int kmod_check_fp(hld_link *L)
{
    osec *o;
    unsigned long total = 0;

    for (o = L->osecs; o; o = o->next) {
        isec *in;
        if (!(o->flags & SHF_EXECINSTR) || o->type == SHT_NOBITS) continue;
        for (in = o->first; in; in = in->next) {
            const uint8_t *code;
            uint64_t b;
            char err[HLD_ERRSZ];

            if (!in->sh || !in->sh->size) continue;
            code = hld_sec_data(in->obj, in->sh, err);
            if (!code) { snprintf(L->err, HLD_ERRSZ, "%s", err); return -1; }
            for (b = 0; b + 16 <= in->sh->size; b += 16) {
                unsigned slot;
                for (slot = 0; slot < 3; slot++) {
                    unsigned fr;
                    char fn[128];
                    uint64_t delta = 0;
                    const char *f;

                    if (!hld_kmod_fp_slot(code + b, slot, &fr)) continue;
                    if (total++ >= 20) continue;
                    f = func_at(in->obj, in->idx, b, &delta, fn, sizeof fn);
                    if (f)
                        fprintf(stderr, "hld: %s: %s+0x%llx (%s+0x%llx): f%u "
                                "is outside the reduced floating-point set "
                                "(f0-f1, f6-f11)\n", in->obj->path,
                                in->sh->name, (unsigned long long)(b + slot),
                                f, (unsigned long long)(delta + slot), fr);
                    else
                        fprintf(stderr, "hld: %s: %s+0x%llx: f%u is outside "
                                "the reduced floating-point set (f0-f1, "
                                "f6-f11)\n", in->obj->path, in->sh->name,
                                (unsigned long long)(b + slot), fr);
                }
            }
        }
    }
    if (total) {
        snprintf(L->err, HLD_ERRSZ, "--kernel-module=11.31: %lu instruction(s)"
                 " use floating-point registers the REDUCEDFP model of an "
                 "11.31 kernel object excludes; build with "
                 "-mfixed-range=f2-f5,f12-f127 (gcc)", total);
        return -1;
    }
    return 0;
}

int hld_write_rel(hld_link *L)
{
    osec **ord = NULL, *o, *comment = NULL;
    size_t nord = 0, i, oi;
    symmap *maps = NULL;
    rstr str, shstr;
    uint8_t *symbuf = NULL, *img = NULL, *shtab = NULL;
    size_t nsym = 0, symcap = 0;
    uint32_t nlocal, nsec, symtab_ndx, strtab_ndx, shstr_ndx;
    uint32_t *secname = NULL;
    uint32_t n_symtab, n_strtab, n_shstrtab;
    hld_gsym **gbyidx = NULL;
    uint64_t off, symoff, stroff, shstroff, shoff, total;
    uint8_t **relbuf = NULL;
    uint64_t *relfill = NULL;
    hld_gsym *g;
    unsigned h;
    kmod_tally kt;
    FILE *f;
    int rc = -1;

    memset(&str, 0, sizeof str);
    memset(&shstr, 0, sizeof shstr);
    memset(&kt, 0, sizeof kt);

    if (!L->nobjs) {
        rerr(L, "no objects to combine", NULL, NULL);
        return -1;
    }

    /*
     * The stamp saying which linker built this, as for every other output
     * (`what' finds it). Appended to .comment, which is not loaded.
     */
    comment = osec_find_pub(L, ".comment");
    if (!comment) {
        comment = osec_get(L, ".comment", SHT_PROGBITS, 0);
        if (!comment) goto oom;
        comment->order = (int)L->nosecs;
    }

    /* -u names, and anything else interned without an object, in hash order */
    for (h = 0; h < HLD_SYMHASH; h++)
        for (g = L->hash[h]; g; g = g->next) rel_list(L, g);

    /* --- section order and contents ------------------------------------ */
    ord = malloc(L->nosecs * sizeof *ord);
    if (!ord) goto oom;
    for (o = L->osecs; o; o = o->next) ord[nord++] = o;
    qsort(ord, nord, sizeof *ord, rel_order_cmp);

    for (i = 0; i < nord; i++) {
        isec *in;
        size_t extra = 0;

        o = ord[i];
        if (o == comment) extra = strlen(HLD_IDENT) + 1;
        if (o->type == SHT_NOBITS) continue;
        o->data = calloc(1, (size_t)(o->size + extra) + 1);
        if (!o->data) goto oom;
        for (in = o->first; in; in = in->next) {
            const uint8_t *src;
            char err[HLD_ERRSZ];
            if (!in->sh || in->sh->type == SHT_NOBITS || !in->sh->size) continue;
            src = hld_sec_data(in->obj, in->sh, err);
            if (!src) { snprintf(L->err, HLD_ERRSZ, "%s", err); goto done; }
            memcpy(o->data + in->out_off, src, (size_t)in->sh->size);
        }
        if (extra) {
            memcpy(o->data + o->size, HLD_IDENT, extra);
            o->size += extra;
        }
    }

    /* --- symbol maps and relocation counts ------------------------------ */
    maps = calloc(L->nobjs, sizeof *maps);
    if (!maps) goto oom;
    for (oi = 0; oi < L->nobjs; oi++) {
        hld_elf *e = L->objs[oi];
        uint32_t j, *tail;

        maps[oi].relfor = calloc(e->eh.shnum + 1, sizeof *maps[oi].relfor);
        maps[oi].relnext = calloc(e->eh.shnum + 1, sizeof *maps[oi].relnext);
        if (!maps[oi].relfor || !maps[oi].relnext) goto oom;
        for (j = 1; j < e->eh.shnum; j++) {
            hld_shdr *rsh = &e->shdrs[j];
            isec *tin;

            if (rsh->type == SHT_SYMTAB) maps[oi].symtab = j;
            if (rsh->type != SHT_RELA) continue;
            tin = rsh->info < e->eh.shnum
                ? (isec *)e->isec_by_shndx[rsh->info] : NULL;
            if (!tin) {
                rerr(L, "%s: relocation section `%s' applies to a section "
                        "that is not carried into the output", e->path,
                        rsh->name);
                goto done;
            }
            if (rsh->entsize != RELA64_SIZE || rsh->size % RELA64_SIZE) {
                rerr(L, "%s: relocation section `%s' is malformed", e->path,
                     rsh->name);
                goto done;
            }
            if (rsh->link >= e->eh.shnum
                || e->shdrs[rsh->link].type != SHT_SYMTAB) {
                rerr(L, "%s: relocation section `%s' has no symbol table",
                     e->path, rsh->name);
                goto done;
            }
            tin->out->nrela += rsh->size / RELA64_SIZE;
            for (tail = &maps[oi].relfor[rsh->info]; *tail;
                 tail = &maps[oi].relnext[*tail]) ;
            *tail = j;
        }
    }

    /* --- section indices ------------------------------------------------ */
    nsec = 1;
    for (i = 0; i < nord; i++) {
        ord[i]->shndx = nsec++;
        if (ord[i]->nrela) ord[i]->rela_shndx = nsec++;
    }
    symtab_ndx = nsec++;
    strtab_ndx = nsec++;
    shstr_ndx = nsec++;
    if (nsec >= SHN_LORESERVE) {
        rerr(L, "too many output sections for a section header table", NULL, NULL);
        goto done;
    }

    /* --- symbol table --------------------------------------------------- */
    symcap = 64;
    symbuf = calloc(symcap, SYM64_SIZE);
    if (!symbuf) goto oom;
    nsym = 1;
#define SYMROOM() do { \
        if (nsym == symcap) { \
            uint8_t *nb_ = realloc(symbuf, symcap * 2 * SYM64_SIZE); \
            if (!nb_) goto oom; \
            memset(nb_ + symcap * SYM64_SIZE, 0, symcap * SYM64_SIZE); \
            symbuf = nb_; symcap *= 2; \
        } } while (0)

    /*
     * One named section symbol per output section that holds something an
     * input could refer to -- relocations against any input section symbol
     * are moved onto these.
     */
    for (i = 0; i < nord; i++) {
        SYMROOM();
        o = ord[i];
        o->symidx = (uint32_t)nsym;
        hld_put_sym(0, symbuf + nsym * SYM64_SIZE, rstr_add(L, &str, o->name),
                    ELF64_ST_INFO(STB_LOCAL, STT_SECTION), 0,
                    (uint16_t)o->shndx, 0, 0);
        nsym++;
    }

    /* each input's locals, its FILE symbol first as in its own table */
    for (oi = 0; oi < L->nobjs; oi++) {
        hld_elf *e = L->objs[oi];
        hld_sym *syms;
        size_t n, k;
        char err[HLD_ERRSZ];

        if (!maps[oi].symtab) continue;
        syms = hld_read_syms(e, &e->shdrs[maps[oi].symtab], &n, err);
        if (!syms) { snprintf(L->err, HLD_ERRSZ, "%s", err); goto done; }
        maps[oi].n = n;
        maps[oi].idx = calloc(n ? n : 1, sizeof *maps[oi].idx);
        maps[oi].adj = calloc(n ? n : 1, sizeof *maps[oi].adj);
        if (!maps[oi].idx || !maps[oi].adj) { free(syms); goto oom; }

        for (k = 1; k < n; k++) {
            hld_sym *s = &syms[k];
            uint8_t type = ELF64_ST_TYPE(s->info);
            uint16_t shndx = s->shndx;
            uint64_t value = s->value;
            isec *in = NULL;

            if (ELF64_ST_BIND(s->info) != STB_LOCAL) continue;
            if (shndx != SHN_UNDEF && shndx < SHN_LORESERVE) {
                in = hld_isec_of(L, e, shndx);
                if (!in) {
                    rerr(L, "%s: local symbol `%s' is in a section that is "
                            "not carried into the output", e->path, s->name);
                    free(syms);
                    goto done;
                }
            }
            if (type == STT_SECTION) {
                /* replaced by the output section's own symbol */
                if (!in) {
                    rerr(L, "%s: section symbol `%s' names no section",
                         e->path, s->name);
                    free(syms);
                    goto done;
                }
                maps[oi].idx[k] = in->out->symidx;
                maps[oi].adj[k] = in->out_off + s->value;
                continue;
            }
            if (shndx == SHN_COMMON || shndx == SHN_IA_64_ANSI_COMMON
                || (shndx >= SHN_LORESERVE && shndx != SHN_ABS)) {
                rerr(L, "%s: local symbol `%s' has a section index hld -r "
                        "does not know", e->path, s->name);
                free(syms);
                goto done;
            }
            if (in) {
                shndx = (uint16_t)in->out->shndx;
                value += in->out_off;
            }
            SYMROOM();
            maps[oi].idx[k] = (uint32_t)nsym;
            hld_put_sym(0, symbuf + nsym * SYM64_SIZE,
                        rstr_add(L, &str, s->name), s->info, s->other, shndx,
                        value, s->size);
            nsym++;
        }
        free(syms);
    }
    nlocal = (uint32_t)nsym;

    /* the globals, in the order the inputs first named them */
    for (g = L->rsyms; g; g = g->r_next) {
        uint16_t shndx = SHN_UNDEF;
        uint64_t value = 0;
        uint8_t bind = g->bind ? g->bind : STB_GLOBAL;

        switch (g->kind) {
        case HLD_SYM_DEFINED:
            shndx = (uint16_t)g->in->out->shndx;
            value = g->in->out_off + g->in_off;
            break;
        case HLD_SYM_ABS:
            shndx = SHN_ABS;
            value = g->value;
            break;
        case HLD_SYM_COMMON:
            shndx = g->short_common ? SHN_IA_64_ANSI_COMMON : SHN_COMMON;
            value = g->align;          /* a COMMON's value is its alignment */
            break;
        default:
            /*
             * Left for the kernel (or the next link) to bind. A kernel
             * module's references to kernel functions all end up here.
             */
            break;
        }
        SYMROOM();
        g->r_symidx = (uint32_t)nsym;
        hld_put_sym(0, symbuf + nsym * SYM64_SIZE, rstr_add(L, &str, g->name),
                    ELF64_ST_INFO(bind, g->type), g->other, shndx, value,
                    g->size);
        nsym++;
    }
#undef SYMROOM
    if (L->err[0]) goto done;          /* a string table allocation failed */
    gbyidx = calloc(nsym, sizeof *gbyidx);
    if (!gbyidx) goto oom;
    for (g = L->rsyms; g; g = g->r_next) gbyidx[g->r_symidx] = g;

    /* the inputs' globals now have somewhere to point */
    for (oi = 0; oi < L->nobjs; oi++) {
        hld_elf *e = L->objs[oi];
        hld_sym *syms;
        size_t n, k;
        char err[HLD_ERRSZ];

        if (!maps[oi].symtab) continue;
        syms = hld_read_syms(e, &e->shdrs[maps[oi].symtab], &n, err);
        if (!syms) { snprintf(L->err, HLD_ERRSZ, "%s", err); goto done; }
        for (k = 1; k < n; k++) {
            hld_sym *s = &syms[k];
            if (ELF64_ST_BIND(s->info) == STB_LOCAL || !s->name[0]) continue;
            g = hld_sym_lookup(L, s->name);
            if (g) maps[oi].idx[k] = g->r_symidx;
        }
        free(syms);
    }

    /* --- relocations ---------------------------------------------------- */
    relbuf = calloc(nord, sizeof *relbuf);
    relfill = calloc(nord, sizeof *relfill);
    if (!relbuf || !relfill) goto oom;
    for (i = 0; i < nord; i++) {
        isec *in;

        o = ord[i];
        if (!o->nrela) continue;
        relbuf[i] = calloc((size_t)o->nrela, RELA64_SIZE);
        if (!relbuf[i]) goto oom;
        /*
         * In contribution order, so the output's relocations ascend through
         * the section as each input's did.
         */
        for (in = o->first; in; in = in->next) {
            hld_elf *e = in->obj;
            uint32_t j;
            size_t mi;

            for (mi = 0; mi < L->nobjs && L->objs[mi] != e; mi++) ;
            for (j = maps[mi].relfor[in->idx]; j; j = maps[mi].relnext[j]) {
                hld_shdr *rsh = &e->shdrs[j];
                hld_rela *rel;
                size_t nrel, k;
                char err[HLD_ERRSZ];

                rel = hld_read_relas(e, rsh, &nrel, err);
                if (!rel) { snprintf(L->err, HLD_ERRSZ, "%s", err); goto done; }
                for (k = 0; k < nrel; k++) {
                    const hld_rela *r = &rel[k];
                    uint8_t *p = relbuf[i] + relfill[i] * RELA64_SIZE;
                    uint32_t sym = 0;
                    uint64_t adj = 0;

                    if (r->sym) {
                        if (r->sym >= maps[mi].n || !maps[mi].idx[r->sym]) {
                            rerr(L, "%s: a relocation in `%s' names a symbol "
                                    "that is not in its symbol table",
                                 e->path, rsh->name);
                            free(rel);
                            goto done;
                        }
                        sym = maps[mi].idx[r->sym];
                        adj = maps[mi].adj[r->sym];
                    }
                    if (L->kmod_check) {
                        const char *nm = "";
                        if (sym >= nlocal) {
                            if (gbyidx[sym]) nm = gbyidx[sym]->name;
                        } else if (sym) {
                            nm = str.buf + be32(symbuf + sym * SYM64_SIZE);
                        }
                        kmod_note(&kt, in, r, nm, r->addend + (int64_t)adj);
                    }
                    st64(p + 0, in->out_off + r->offset);
                    st64(p + 8, ELF64_R_INFO(sym, r->type));
                    st64(p + 16, (uint64_t)r->addend + adj);
                    relfill[i]++;
                }
                free(rel);
            }
        }
    }

    /* --- the kernel loader's view of it, when asked --------------------- */
    if (L->kmod_target == 1131 && kmod_check_fp(L) < 0) goto done;
    if (L->kmod_check) {
        osec *shtmod = osec_find_pub(L, ".shtmod");
        int have_wrapper = 0;

        if (kt.refused || kt.milli) {
            if (kt.refused + kt.milli > 25)
                fprintf(stderr, "hld: ... and %lu more\n",
                        kt.refused + kt.milli - 25);
            snprintf(L->err, HLD_ERRSZ, "--kernel-module: %lu relocation(s) "
                     "the module loader does not process, %lu call(s) to "
                     "millicode; compile for the kernel (HP cc "
                     "-Wc,-codeconvention,esnonreloc; gcc -mno-pic "
                     "-minline-int-divide-max-throughput)", kt.refused,
                     kt.milli);
            goto done;
        }
        for (g = L->rsyms; g; g = g->r_next)
            if (g->kind == HLD_SYM_UNDEF && strncmp(g->name, "__milli", 7) == 0) {
                rerr(L, "--kernel-module: the module refers to millicode "
                        "(`%s'), which the kernel does not export to modules",
                     g->name, NULL);
                goto done;
            }
        if (!shtmod) {
            rerr(L, "--kernel-module: no .shtmod section, so the module "
                    "loader would refuse it (\"Non-existent or invalid "
                    "SHT_MOD section\")", NULL, NULL);
            goto done;
        }
        for (i = 0; i < nord; i++) {
            uint64_t k;
            if (ord[i] != shtmod) continue;
            for (k = 0; k < relfill[i]; k++)
                if (be64(relbuf[i] + k * RELA64_SIZE) == 16
                    && (uint32_t)be64(relbuf[i] + k * RELA64_SIZE + 8)
                       == R_IA64_DIR64MSB)
                    have_wrapper = 1;
        }
        if (shtmod->size < 24 || !have_wrapper) {
            rerr(L, "--kernel-module: .shtmod holds no pointer to the module "
                    "wrapper (a DIR64MSB relocation at offset 16)", NULL, NULL);
            goto done;
        }
        if (!osec_find_pub(L, ".data.KC_META")
            || !osec_find_pub(L, ".data.KC_META_PTR")) {
            rerr(L, "--kernel-module: no .data.KC_META/.data.KC_META_PTR "
                    "sections; kcmodule finds a module by them", NULL, NULL);
            goto done;
        }
        if (!(L->rflags & EF_IA_64_NOFUNCDESC_CONS_GP))
            fprintf(stderr, "hld: warning: --kernel-module: the inputs were "
                    "not compiled for the kernel (e_flags lack "
                    "NOFUNCDESC_CONS_GP, 0x80)\n");
        if (kt.unseen)
            fprintf(stderr, "hld: warning: --kernel-module: %lu relocation(s)"
                    " of a type no module known to load has, first %s\n",
                    kt.unseen, kt.first_unseen);
    }

    /* --- file layout ---------------------------------------------------- */
    off = EHDR64_SIZE;
    for (i = 0; i < nord; i++) {
        o = ord[i];
        off = align_up(off, o->align ? o->align : 1);
        o->off = off;
        if (o->type != SHT_NOBITS) off += o->size;
        if (o->nrela) {
            off = align_up(off, 8);
            o->addr = off;              /* (the .rela's offset; no addresses here) */
            off += o->nrela * RELA64_SIZE;
        }
    }
    off = align_up(off, 8);
    symoff = off;
    off += nsym * SYM64_SIZE;
    stroff = off;
    off += str.len;

    secname = calloc(nsec, sizeof *secname);
    if (!secname) goto oom;
    for (i = 0; i < nord; i++) {
        char *rn;
        o = ord[i];
        secname[o->shndx] = rstr_add(L, &shstr, o->name);
        if (!o->nrela) continue;
        rn = malloc(strlen(o->name) + 6);
        if (!rn) goto oom;
        sprintf(rn, ".rela%s", o->name);
        secname[o->rela_shndx] = rstr_add(L, &shstr, rn);
        free(rn);
    }
    n_symtab = rstr_add(L, &shstr, ".symtab");
    n_strtab = rstr_add(L, &shstr, ".strtab");
    n_shstrtab = rstr_add(L, &shstr, ".shstrtab");
    if (L->err[0]) goto done;
    shstroff = off;
    off += shstr.len;
    shoff = align_up(off, 8);
    total = shoff + (uint64_t)nsec * SHDR64_SIZE;

    img = calloc(1, (size_t)total);
    if (!img) goto oom;

    /* --- ELF header ----------------------------------------------------- */
    memcpy(img, ELFMAG, 4);
    img[4] = ELFCLASS64;
    img[5] = ELFDATA2MSB;
    img[6] = EV_CURRENT;
    img[7] = ELFOSABI_HPUX;
    img[8] = HPUX_ABIVERSION;
    st16(img + 16, ET_REL);
    st16(img + 18, EM_IA_64);
    st32(img + 20, EV_CURRENT);
    st64(img + 40, shoff);
    st32(img + 48, rel_out_flags(L));
    st16(img + 52, EHDR64_SIZE);
    st16(img + 54, PHDR64_SIZE);
    st16(img + 58, SHDR64_SIZE);
    st16(img + 60, (uint16_t)nsec);
    st16(img + 62, (uint16_t)shstr_ndx);

    /* --- contents and section headers ----------------------------------- */
    shtab = img + shoff;
    for (i = 0; i < nord; i++) {
        uint32_t type, link = 0, info = 0;
        uint64_t flags, entsize;

        o = ord[i];
        type = o->type;
        flags = o->flags;
        entsize = o->entsize;
        if (o->type != SHT_NOBITS && o->size)
            memcpy(img + o->off, o->data, (size_t)o->size);
        /*
         * With --kernel-module, .shtmod comes out as SHT_MOD, the type the
         * module loader demands ("Non-existent or invalid SHT_MOD section").
         * The assembler can only make PROGBITS and HP ld -r leaves it so --
         * a separate script patches it afterwards. Plain -r keeps HP's
         * behaviour: an input already of type SHT_MOD stays SHT_MOD, a
         * PROGBITS one stays PROGBITS.
         */
        if (is_shtmod(o->name) && L->kmod_check) type = SHT_HP_MOD;
        if (type == SHT_IA_64_UNWIND) {
            flags |= SHF_LINK_ORDER;
            info = o->assoc ? o->assoc->shndx : 0;
            entsize = 4;
        }
        hld_put_shdr(0, shtab + (size_t)o->shndx * SHDR64_SIZE,
                     secname[o->shndx], type, flags, 0, o->off, o->size,
                     link, info, o->align, entsize);
        if (o->nrela) {
            memcpy(img + o->addr, relbuf[i], (size_t)(o->nrela * RELA64_SIZE));
            hld_put_shdr(0, shtab + (size_t)o->rela_shndx * SHDR64_SIZE,
                         secname[o->rela_shndx], SHT_RELA, 0, 0, o->addr,
                         o->nrela * RELA64_SIZE, symtab_ndx, o->shndx, 8,
                         RELA64_SIZE);
        }
    }
    memcpy(img + symoff, symbuf, nsym * SYM64_SIZE);
    memcpy(img + stroff, str.buf, str.len);
    memcpy(img + shstroff, shstr.buf, shstr.len);
    hld_put_shdr(0, shtab + (size_t)symtab_ndx * SHDR64_SIZE, n_symtab,
                 SHT_SYMTAB, 0, 0, symoff, nsym * SYM64_SIZE, strtab_ndx,
                 nlocal, 8, SYM64_SIZE);
    hld_put_shdr(0, shtab + (size_t)strtab_ndx * SHDR64_SIZE, n_strtab,
                 SHT_STRTAB, 0, 0, stroff, str.len, 0, 0, 1, 0);
    hld_put_shdr(0, shtab + (size_t)shstr_ndx * SHDR64_SIZE, n_shstrtab,
                 SHT_STRTAB, 0, 0, shstroff, shstr.len, 0, 0, 1, 0);

    f = fopen(L->out_path, "wb");
    if (!f) {
        snprintf(L->err, HLD_ERRSZ, "%s: cannot create output", L->out_path);
        goto done;
    }
    /* a half-written object must not be mistaken for a module */
    if (fwrite(img, 1, (size_t)total, f) != (size_t)total) {
        snprintf(L->err, HLD_ERRSZ, "%s: write failed", L->out_path);
        fclose(f);
        remove(L->out_path);
        goto done;
    }
    if (fclose(f) != 0) {
        snprintf(L->err, HLD_ERRSZ, "%s: write failed", L->out_path);
        remove(L->out_path);
        goto done;
    }
    if (L->verbose)
        fprintf(stderr, "hld: -r: %u sections, %lu symbols (%u local), "
                "e_flags 0x%x\n", (unsigned)nsec, (unsigned long)nsym,
                (unsigned)nlocal, (unsigned)rel_out_flags(L));
    rc = 0;
    goto done;

oom:
    rerr(L, "out of memory", NULL, NULL);
done:
    if (maps) {
        for (oi = 0; oi < L->nobjs; oi++) {
            free(maps[oi].idx);
            free(maps[oi].adj);
            free(maps[oi].relfor);
            free(maps[oi].relnext);
        }
        free(maps);
    }
    if (relbuf) {
        for (i = 0; i < nord; i++) free(relbuf[i]);
        free(relbuf);
    }
    free(relfill);
    free(secname);
    free(ord);
    free(symbuf);
    free(str.buf);
    free(shstr.buf);
    free(img);
    free(gbyidx);
    return rc;
}
