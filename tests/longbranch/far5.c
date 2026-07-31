#include <stdio.h>
int t1(void){return 1;} 
asm(".text\n.skip 0x1100000,0\n");   /* 17 MB */
int t2(void){return 2;}
asm(".text\n.skip 0x0300000,0\n");   /*  3 MB */
int t3(void){return 3;}
asm(".text\n.skip 0x0400000,0\n");   /*  4 MB */
int t4(void){return 4;}
asm(".text\n.skip 0x0600000,0\n");   /*  6 MB */
int t5(void){return 5;}
asm(".text\n.skip 0x0A00000,0\n");   /* 10 MB */
int caller(void){ return t1()+t2()+t3()+t4()+t5(); }
int main(void){ int v=caller(); printf("sum=%d\n",v); return v==15?0:1; }
