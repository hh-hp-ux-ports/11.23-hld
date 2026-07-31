#include <stdio.h>

/* Callee placed FIRST in .text */
int target(void) { return 42; }

/* ~20 MB of padding, emitted between callee and caller, so that the call
   below has a displacement well beyond PCREL21B (+/-16 MB) and therefore
   requires a long-branch stub. */
asm(".text\n.skip 0x1400000, 0\n");

/* Caller placed LAST in .text */
int caller(void) { return target(); }

int main(void) {
  int v = caller();
  printf("target() returned %d\n", v);
  return v == 42 ? 0 : 1;
}
