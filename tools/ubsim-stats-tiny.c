/* Libc-free, UART-transferable gem5 stats trigger for native ARM64 guests. */
#include <gem5/m5ops.h>

__attribute__((used)) static int
run(long argc, char **argv)
{
    if (argc != 2)
        return 2;
    if (argv[1][0] == 'r' && argv[1][1] == '\0') {
        m5_reset_stats(0, 0);
        return 0;
    }
    if (argv[1][0] == 'd' && argv[1][1] == '\0') {
        m5_dump_stats(0, 0);
        return 0;
    }
    return 2;
}

__attribute__((naked)) void
_start(void)
{
    __asm__ volatile(
        "ldr x0, [sp]\n"
        "add x1, sp, #8\n"
        "bl run\n"
        "mov x8, #93\n"
        "svc #0\n");
}
