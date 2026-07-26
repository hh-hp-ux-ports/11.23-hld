/*
 * patch_harness — golden-pair validation of hld_ia64_install_value.
 *
 * pair mode: two objects assembled from identical sources differing only in
 * one immediate/displacement. Patch A's bundle with B's value; the result
 * must equal B's bundle byte-for-byte (gas is the encoding oracle).
 *
 *   patch_harness pair A.o offA B.o offB slot rtype val [ok|overflow]
 *   patch_harness selftest
 */
#include "../src/port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/elf64.h"
#include "../src/ia64_patch.h"

#define U(x) ((unsigned long long)(x))

static const uint8_t *text_bytes(const char *path, uint64_t off, hld_elf **out)
{
    char err[HLD_ERRSZ];
    hld_elf *e = hld_elf_load(path, err);
    const uint8_t *d;
    uint32_t i;

    if (!e) { fprintf(stderr, "%s\n", err); return NULL; }
    for (i = 0; i < e->eh.shnum; i++)
        if (strcmp(e->shdrs[i].name, ".text") == 0) {
            if (off + 16 > e->shdrs[i].size) {
                fprintf(stderr, "%s: .text too small for bundle @0x%llx\n",
                        path, U(off));
                hld_elf_free(e);
                return NULL;
            }
            d = hld_sec_data(e, &e->shdrs[i], err);
            if (!d) { fprintf(stderr, "%s\n", err); hld_elf_free(e); return NULL; }
            *out = e;
            return d + off;
        }
    fprintf(stderr, "%s: no .text\n", path);
    hld_elf_free(e);
    return NULL;
}

static void hexdump16(const char *tag, const uint8_t *p)
{
    int i;
    fprintf(stderr, "  %-8s", tag);
    for (i = 0; i < 16; i++) fprintf(stderr, " %02x", p[i]);
    fprintf(stderr, "\n");
}

static int do_pair(int argc, char **argv)
{
    const char *pa, *pb, *expect;
    uint64_t offa, offb, val;
    unsigned slot;
    uint32_t rtype;
    hld_elf *ea = NULL, *eb = NULL;
    const uint8_t *a, *b;
    uint8_t bundle[16];
    hld_patch_status st;

    if (argc < 8) { fprintf(stderr, "pair: bad args\n"); return 2; }
    pa    = argv[1];
    offa  = strtoull(argv[2], NULL, 0);
    pb    = argv[3];
    offb  = strtoull(argv[4], NULL, 0);
    slot  = (unsigned)strtoul(argv[5], NULL, 0);
    rtype = (uint32_t)strtoul(argv[6], NULL, 0);
    val   = strtoull(argv[7], NULL, 0);
    expect = argc > 8 ? argv[8] : "ok";

    a = text_bytes(pa, offa, &ea);
    if (!a) return 1;
    b = text_bytes(pb, offb, &eb);
    if (!b) { hld_elf_free(ea); return 1; }

    memcpy(bundle, a, 16);
    st = hld_ia64_install_value(bundle, slot, val, rtype);

    if (strcmp(expect, "overflow") == 0) {
        if (st != HLD_PATCH_OVERFLOW) {
            fprintf(stderr, "FAIL: expected overflow, got status %d\n", st);
            hld_elf_free(ea); hld_elf_free(eb);
            return 1;
        }
    } else {
        if (st != HLD_PATCH_OK) {
            fprintf(stderr, "FAIL: install status %d\n", st);
            hld_elf_free(ea); hld_elf_free(eb);
            return 1;
        }
        if (memcmp(bundle, b, 16) != 0) {
            fprintf(stderr,
                    "FAIL: patched A != B (slot %u rtype 0x%x val 0x%llx)\n",
                    slot, rtype, U(val));
            hexdump16("A:", a);
            hexdump16("patched:", bundle);
            hexdump16("B:", b);
            hld_elf_free(ea); hld_elf_free(eb);
            return 1;
        }
    }
    hld_elf_free(ea);
    hld_elf_free(eb);
    return 0;
}

static int do_selftest(void)
{
    uint8_t buf[8];
    static const uint8_t be64v[8] =
        { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    static const uint8_t be32v[4] = { 0xde, 0xad, 0xbe, 0xef };
    static const uint8_t le64v[8] =
        { 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01 };

    memset(buf, 0, 8);
    if (hld_ia64_install_value(buf, 0, 0x0123456789abcdefULL, R_IA64_DIR64MSB)
        != HLD_PATCH_OK || memcmp(buf, be64v, 8) != 0) {
        fprintf(stderr, "FAIL: DIR64MSB\n");
        return 1;
    }
    memset(buf, 0, 8);
    if (hld_ia64_install_value(buf, 0, 0xdeadbeefULL, R_IA64_SEGREL32MSB)
        != HLD_PATCH_OK || memcmp(buf, be32v, 4) != 0) {
        fprintf(stderr, "FAIL: SEGREL32MSB\n");
        return 1;
    }
    memset(buf, 0, 8);
    if (hld_ia64_install_value(buf, 0, 0x0123456789abcdefULL, R_IA64_DIR64LSB)
        != HLD_PATCH_OK || memcmp(buf, le64v, 8) != 0) {
        fprintf(stderr, "FAIL: DIR64LSB\n");
        return 1;
    }
    /* policy returns */
    if (hld_ia64_install_value(buf, 0, 0, R_IA64_LDXMOV) != HLD_PATCH_OK
        || hld_ia64_install_value(buf, 0, 0, R_IA64_NONE) != HLD_PATCH_OK
        || hld_ia64_install_value(buf, 2, 0, R_IA64_IMM64) != HLD_PATCH_BADSLOT
        || hld_ia64_install_value(buf, 0, 0, R_IA64_IPLTMSB)
           != HLD_PATCH_UNSUPPORTED) {
        fprintf(stderr, "FAIL: status policy\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0)
        return do_selftest();
    if (argc >= 2 && strcmp(argv[1], "pair") == 0)
        return do_pair(argc - 1, argv + 1);
    fprintf(stderr,
        "usage: patch_harness selftest\n"
        "       patch_harness pair A.o offA B.o offB slot rtype val [ok|overflow]\n");
    return 2;
}
