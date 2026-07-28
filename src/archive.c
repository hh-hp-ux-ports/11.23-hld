/*
 * archive.c — `ar` archives as linker input.
 *
 * An archive is searched at its position on the command line: members are
 * pulled in only when they define a symbol that is undefined at that moment,
 * and pulling one member can make a sibling necessary, so each search repeats
 * until a pass extracts nothing.
 *
 * The archive's own symbol index is ignored. Instead each member's symbol
 * table is read once and its defined globals recorded, which sidesteps the
 * several incompatible index formats (SysV 32-bit, /SYM64/, BSD __.SYMDEF)
 * for the cost of one pass over members we would have to parse anyway.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"

#define ARMAG     "!<arch>\n"
#define ARMAG_LEN 8
#define THINMAG   "!<thin>\n"
#define ARHDR_LEN 60
#define ARFMAG    "`\n"

static void aerr(hld_link *L, const char *fmt, const char *a, const char *b)
{
    snprintf(L->err, HLD_ERRSZ, fmt, a ? a : "", b ? b : "");
}

/* Header fields are ASCII, blank padded, and not NUL terminated. */
static long ar_num(const uint8_t *p, size_t n)
{
    char buf[32];
    size_t i;

    if (n >= sizeof buf) n = sizeof buf - 1;
    for (i = 0; i < n; i++) buf[i] = (char)p[i];
    buf[n] = 0;
    return strtol(buf, NULL, 10);
}

static unsigned ar_hash(const char *s)
{
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h % HLD_ARHASH;
}

static int ar_index_add(hld_archive *ar, const char *name, size_t member)
{
    unsigned h = ar_hash(name);
    arsym *s = calloc(1, sizeof *s);

    if (!s) return -1;
    s->name = name;              /* into the member's own string table */
    s->member = member;
    s->next = ar->hash[h];
    ar->hash[h] = s;
    return 0;
}

static size_t ar_index_lookup(hld_archive *ar, const char *name, int *found)
{
    arsym *s;

    for (s = ar->hash[ar_hash(name)]; s; s = s->next)
        if (strcmp(s->name, name) == 0) {
            *found = 1;
            return s->member;
        }
    *found = 0;
    return 0;
}

/*
 * Record the defined globals of one member. The member is parsed with the
 * archive's buffer borrowed; the parsed form is kept so extraction does not
 * have to parse it again, and so the recorded names stay valid.
 */
static int index_member(hld_link *L, hld_archive *ar, size_t mi)
{
    armember *m = &ar->members[mi];
    char err[HLD_ERRSZ];
    uint32_t j;

    /* Not an object at all (a stray text file, say): nothing to offer. */
    if (m->len < 4 || memcmp(ar->data + m->off, ELFMAG, 4) != 0)
        return 0;

    m->elf = hld_elf_from_memory(m->name, ar->data + m->off, m->len, 0, err);
    if (!m->elf)
        return 0;               /* unreadable member: it defines nothing */

    for (j = 1; j < m->elf->eh.shnum; j++) {
        hld_sym *syms;
        size_t n, k;

        if (m->elf->shdrs[j].type != SHT_SYMTAB) continue;
        syms = hld_read_syms(m->elf, &m->elf->shdrs[j], &n, err);
        if (!syms) continue;
        for (k = 0; k < n; k++) {
            hld_sym *sy = &syms[k];
            if (sy->shndx == SHN_UNDEF || !sy->name[0]) continue;
            if (ELF64_ST_BIND(sy->info) == STB_LOCAL) continue;
            if (ar_index_add(ar, sy->name, mi) < 0) {
                free(syms);
                aerr(L, "out of memory", NULL, NULL);
                return -1;
            }
        }
        free(syms);
    }
    return 0;
}

int hld_archive_open(hld_link *L, const char *path, hld_archive **out)
{
    hld_archive *ar;
    uint8_t *data;
    size_t size, pos, cap = 0;
    const uint8_t *longnames = NULL;
    size_t longnames_len = 0;

    *out = NULL;

    {
        FILE *f = fopen(path, "rb");
        long sz;
        if (!f) { aerr(L, "%s: cannot open", path, NULL); return -1; }
        if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
            aerr(L, "%s: cannot size", path, NULL);
            fclose(f);
            return -1;
        }
        rewind(f);
        data = malloc(sz ? (size_t)sz : 1);
        if (!data) { aerr(L, "out of memory", NULL, NULL); fclose(f); return -1; }
        if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
            aerr(L, "%s: short read", path, NULL);
            free(data);
            fclose(f);
            return -1;
        }
        fclose(f);
        size = (size_t)sz;
    }

    if (size >= ARMAG_LEN && memcmp(data, THINMAG, ARMAG_LEN) == 0) {
        aerr(L, "%s: thin archives are not supported", path, NULL);
        free(data);
        return -1;
    }
    if (size < ARMAG_LEN || memcmp(data, ARMAG, ARMAG_LEN) != 0) {
        aerr(L, "%s: not an archive", path, NULL);
        free(data);
        return -1;
    }

    ar = calloc(1, sizeof *ar);
    if (!ar) { aerr(L, "out of memory", NULL, NULL); free(data); return -1; }
    ar->path = malloc(strlen(path) + 1);
    if (!ar->path) { aerr(L, "out of memory", NULL, NULL); free(data); free(ar); return -1; }
    strcpy(ar->path, path);
    ar->data = data;
    ar->size = size;

    /* --- walk the member headers ------------------------------------- */
    pos = ARMAG_LEN;
    while (pos + ARHDR_LEN <= size) {
        const uint8_t *h = data + pos;
        size_t mlen;
        char rawname[17];
        int i;

        if (memcmp(h + 58, ARFMAG, 2) != 0) {
            aerr(L, "%s: malformed archive member header", path, NULL);
            hld_archive_free(ar);
            return -1;
        }
        mlen = (size_t)ar_num(h + 48, 10);
        pos += ARHDR_LEN;
        if (mlen > size - pos) {
            aerr(L, "%s: archive member runs past end of file", path, NULL);
            hld_archive_free(ar);
            return -1;
        }

        for (i = 0; i < 16; i++) rawname[i] = (char)h[i];
        rawname[16] = 0;
        for (i = 15; i >= 0 && (rawname[i] == ' ' || rawname[i] == '/'); i--)
            rawname[i] = 0;

        if (rawname[0] == 0 && h[0] == '/' && h[1] == ' ') {
            /* the archive's symbol index — we build our own */
        } else if (h[0] == '/' && h[1] == '/') {
            longnames = data + pos;             /* long-name string table */
            longnames_len = mlen;
        } else if (strcmp(rawname, "__.SYMDEF") == 0) {
            /* BSD symbol index — likewise ignored */
        } else {
            armember *m;
            const char *name = NULL;
            size_t off = pos, len = mlen;
            char *owned = NULL;

            if (h[0] == '/' && h[1] >= '0' && h[1] <= '9' && longnames) {
                /* name is an offset into the long-name table */
                size_t no = (size_t)ar_num(h + 1, 15);
                if (no < longnames_len) {
                    const uint8_t *e = longnames + no;
                    size_t l = 0;
                    while (no + l < longnames_len && e[l] != '\n' && e[l] != '/')
                        l++;
                    owned = malloc(l + 1);
                    if (owned) { memcpy(owned, e, l); owned[l] = 0; name = owned; }
                }
            } else if (rawname[0] == '#' && rawname[1] == '1' && rawname[2] == '/') {
                /* BSD: the name occupies the first bytes of the member */
                size_t nl = (size_t)ar_num(h + 3, 13);
                if (nl <= len) {
                    owned = malloc(nl + 1);
                    if (owned) {
                        memcpy(owned, data + off, nl);
                        owned[nl] = 0;
                        name = owned;
                    }
                    off += nl;
                    len -= nl;
                }
            }
            if (!name) {
                owned = malloc(strlen(rawname) + 1);
                if (owned) { strcpy(owned, rawname); name = owned; }
            }
            if (!name) {
                aerr(L, "out of memory", NULL, NULL);
                hld_archive_free(ar);
                return -1;
            }

            if (ar->nmembers == cap) {
                size_t nc = cap ? cap * 2 : 16;
                armember *nm = realloc(ar->members, nc * sizeof *nm);
                if (!nm) {
                    aerr(L, "out of memory", NULL, NULL);
                    free(owned);
                    hld_archive_free(ar);
                    return -1;
                }
                memset(nm + cap, 0, (nc - cap) * sizeof *nm);
                ar->members = nm;
                cap = nc;
            }
            m = &ar->members[ar->nmembers++];
            m->name = name;
            m->off = off;
            m->len = len;
            m->extracted = 0;
            m->elf = NULL;
        }

        pos += mlen;
        if (pos & 1) pos++;                     /* members are 2-byte aligned */
    }

    /* --- build our own symbol index ---------------------------------- */
    {
        size_t mi;
        for (mi = 0; mi < ar->nmembers; mi++)
            if (index_member(L, ar, mi) < 0) {
                hld_archive_free(ar);
                return -1;
            }
    }

    if (!L->archives) { L->archives = ar; L->ar_tail = &ar->next; }
    else { *L->ar_tail = ar; L->ar_tail = &ar->next; }
    L->narchives++;
    *out = ar;
    return 0;
}

/*
 * Search one archive against the currently undefined symbols, repeating until
 * a pass pulls nothing: a member just extracted may itself reference another.
 */
int hld_archive_search(hld_link *L, hld_archive *ar, int *extracted_any)
{
    int changed;
    unsigned h;
    hld_gsym *g;

    if (extracted_any) *extracted_any = 0;
    do {
        changed = 0;
        for (h = 0; h < HLD_SYMHASH; h++) {
            for (g = L->hash[h]; g; g = g->next) {
                size_t mi;
                int found;
                armember *m;

                if (g->kind != HLD_SYM_UNDEF) continue;
                /*
                 * A weak reference does not pull a member in — that is what
                 * makes a weak symbol optional rather than merely late.
                 */
                if (g->bind == STB_WEAK) continue;

                mi = ar_index_lookup(ar, g->name, &found);
                if (!found) continue;
                m = &ar->members[mi];
                if (m->extracted) continue;

                if (!m->elf) {
                    aerr(L, "%s(%s): not a usable object", ar->path, m->name);
                    return -1;
                }
                if (m->elf->eh.type != ET_REL
                    || m->elf->eh.machine != EM_IA_64
                    || !(m->elf->eh.flags & EF_IA_64_ABI64)) {
                    snprintf(L->err, HLD_ERRSZ,
                             "%s(%s): not an LP64 IA-64 object — wrong library "
                             "for this ABI", ar->path, m->name);
                    return -1;
                }
                m->extracted = 1;
                if (hld_input_object(L, m->elf) < 0) return -1;
                changed = 1;
                if (extracted_any) *extracted_any = 1;
            }
        }
    } while (changed);
    return 0;
}

void hld_archive_free(hld_archive *ar)
{
    size_t i;
    unsigned h;

    if (!ar) return;
    for (i = 0; i < ar->nmembers; i++) {
        free((void *)ar->members[i].name);
        /* An extracted member's parsed form is owned by the link. */
        if (ar->members[i].elf && !ar->members[i].extracted)
            hld_elf_free(ar->members[i].elf);
    }
    for (h = 0; h < HLD_ARHASH; h++) {
        arsym *s, *n;
        for (s = ar->hash[h]; s; s = n) { n = s->next; free(s); }
    }
    free(ar->members);
    free(ar->data);
    free(ar->path);
    free(ar);
}
