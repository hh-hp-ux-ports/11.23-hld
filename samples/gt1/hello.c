#include <stdio.h>
int g_init = 42;
int g_bss;
static const char msg[] = "hello-from-hld-groundtruth";
int add3(int a) { return a + 3; }
int main(void) { printf("%s %d %d\n", msg, g_init + add3(1), g_bss); return 0; }
