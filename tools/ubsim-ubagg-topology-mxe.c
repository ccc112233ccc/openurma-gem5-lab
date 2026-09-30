// SPDX-License-Identifier: MIT
/* Supply the explicit two-node lab topology through the official UVS API.
 *
 * The official matrix-server balance provider names these two entries
 * io_die_info[], but consumes them as two data-plane planes: one primary EID
 * plus up to nine port EIDs per plane.  They do not describe the gem5 CPU
 * count.  Keeping both entries is required by the unmodified provider's
 * PRIMARY_EID_NUM == 2 contract.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uvs_api.h"

#define EID_LEN 16
#define MAX_PORT_NUM 9
#define IODIE_NUM 2

struct lab_iodie_info {
    uint8_t primary_eid[EID_LEN];
    uint8_t port_eid[MAX_PORT_NUM][EID_LEN];
    uint8_t peer_port_eid[MAX_PORT_NUM][EID_LEN];
    int socket_id;
};

struct lab_topo_info {
    uint8_t bonding_eid[EID_LEN];
    struct lab_iodie_info io_die_info[IODIE_NUM];
    bool is_cur_node;
};

/* Keep the helper honest about the private MXE-to-UVS ABI consumed by the
 * unmodified official userspace and kernel implementations. */
_Static_assert(sizeof(struct lab_iodie_info) == 308,
               "unexpected ubagg IODIE ABI layout");
_Static_assert(offsetof(struct lab_topo_info, is_cur_node) == 632,
               "unexpected ubagg topology ABI layout");
_Static_assert(sizeof(struct lab_topo_info) == 636,
               "unexpected ubagg topology ABI size");

static void compact_eid(uint8_t out[EID_LEN], uint32_t value)
{
    memset(out, 0, EID_LEN);
    out[13] = (uint8_t)(value >> 16);
    out[14] = (uint8_t)(value >> 8);
    out[15] = (uint8_t)value;
}

static int parse_u20(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (text[0] == '\0' || end == text || *end != '\0' ||
        parsed > 0xfffffUL)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static void fill_node(struct lab_topo_info *topo, uint32_t base,
                      uint32_t peer_base, uint32_t bond, bool current)
{
    compact_eid(topo->bonding_eid, bond);
    topo->is_cur_node = current;
    for (uint32_t plane = 0; plane < IODIE_NUM; ++plane) {
        const uint32_t primary = base + plane * 0x10000U;
        const uint32_t port = base + (plane + 2U) * 0x10000U;
        const uint32_t peer_port = peer_base + (plane + 2U) * 0x10000U;
        compact_eid(topo->io_die_info[plane].primary_eid, primary);
        compact_eid(topo->io_die_info[plane].port_eid[0], port);
        compact_eid(topo->io_die_info[plane].peer_port_eid[0], peer_port);
        topo->io_die_info[plane].socket_id = (int)plane;
    }
}

int main(int argc, char **argv)
{
    uint32_t node, base0, base1, bond0, bond1;
    struct lab_topo_info topo[2] = {0};

    if (argc != 6 || parse_u20(argv[1], &node) != 0 || node > 1 ||
        parse_u20(argv[2], &base0) != 0 ||
        parse_u20(argv[3], &base1) != 0 ||
        parse_u20(argv[4], &bond0) != 0 ||
        parse_u20(argv[5], &bond1) != 0 || base0 == 0 || base1 == 0 ||
        bond0 == 0 || bond1 == 0) {
        fprintf(stderr, "usage: %s NODE BASE0 BASE1 BOND0 BOND1\n", argv[0]);
        return 2;
    }
    if (base0 + 0x30000U > 0xfffffU ||
        base1 + 0x30000U > 0xfffffU) {
        fprintf(stderr, "base EID leaves no room for two primary/port EIDs\n");
        return 2;
    }

    fill_node(&topo[0], base0, base1, bond0, node == 0);
    fill_node(&topo[1], base1, base0, bond1, node == 1);
    if (uvs_set_topo_info(topo, 2) != 0) {
        fprintf(stderr, "official uvs_set_topo_info failed\n");
        return 1;
    }
    printf("official UVS balance topology installed for node %u\n", node);
    printf("  logical aggregate EID : 0x%05x (bonding_dev_0)\n",
           node == 0 ? bond0 : bond1);
    printf("  plane 0 primary/port  : 0x%05x / 0x%05x\n",
           node == 0 ? base0 : base1,
           (node == 0 ? base0 : base1) + 0x20000U);
    printf("  plane 1 primary/port  : 0x%05x / 0x%05x\n",
           (node == 0 ? base0 : base1) + 0x10000U,
           (node == 0 ? base0 : base1) + 0x30000U);
    printf("  note: the two official io_die_info entries are balance planes,"
           " not gem5 CPUs\n");
    return 0;
}
