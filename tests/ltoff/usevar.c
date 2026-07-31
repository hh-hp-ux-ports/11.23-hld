#include <stdio.h>
typedef unsigned (*hashfn_t)(const void *);
extern hashfn_t hashvar;           /* declared as a variable, called like a function */
int main(void){
  const void *p=(const void*)0x12340;
  unsigned v = hashvar(p);         /* indirect call through the variable */
  printf("hashvar(p)=%u %s\n", v, v==0x2468 ? "OK" : "WRONG");
  return v==0x2468 ? 0 : 1;
}
