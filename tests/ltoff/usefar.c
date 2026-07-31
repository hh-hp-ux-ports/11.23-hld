#include <stdio.h>
typedef unsigned (*hashfn_t)(const void *);
extern hashfn_t hashvar;
int main(void){
  const void *p=(const void*)0x12340;
  unsigned v = hashvar(p);
  printf("hashvar(p)=%u %s\n", v, v==0x2468?"OK":"WRONG");
  return v==0x2468?0:1;
}
