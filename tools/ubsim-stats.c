/* Trigger a gem5 statistics boundary from a checkpointed guest shell. */
#include "ubsim-m5ops.h"

int
main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s reset|dump\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "reset") == 0)
        return ubsim_m5ops_reset_stats() == 0 ? 0 : 1;
    if (strcmp(argv[1], "dump") == 0)
        return ubsim_m5ops_dump_stats() == 0 ? 0 : 1;
    fprintf(stderr, "unknown statistics operation: %s\n", argv[1]);
    return 2;
}
