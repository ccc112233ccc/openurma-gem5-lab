/* Official URMA API end-to-end data/completion checker; not a bandwidth test.
 * Both peers must use the same executable/ABI and arguments (except peer IP).
 * Control TCP carries metadata/barriers only, never the payload under test.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "urma_api.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d expression=%s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
enum { GUARD = 32, MAX_BATCH = 512, MAX_JETTYS = 4 };
static urma_token_t token = {.token = 0xabcdef};
static int sock;
static uint64_t session_seed;
static void transfer(void *buffer, size_t size, int output)
{
    char *p = buffer;
    while (size) {
        ssize_t n = output ? send(sock, p, size, MSG_NOSIGNAL) : recv(sock, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0); p += n; size -= n;
    }
}
static void barrier(void)
{
    unsigned char c = 0x93;
    transfer(&c, 1, 1); transfer(&c, 1, 0); CHECK(c == 0x93);
}
static uint8_t pattern(size_t offset, uint64_t sequence)
{
    /* All values nonzero, varied across offsets, rounds and slots. */
    uint64_t x = offset + sequence * UINT64_C(0x9e3779b97f4a7c15) + session_seed;
    x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb); x ^= x >> 31;
    return (uint8_t)(1 + x % 255);
}
static void verify(uint8_t *p, size_t len, uint64_t seq, int payload)
{
    for (size_t i = 0; i < GUARD; ++i) CHECK(p[-GUARD + (int)i] == 0xa5 && p[len+i] == 0xa5);
    if (!payload) return;
    for (size_t i = 0; i < len; ++i) {
        if (p[i] != pattern(i, seq)) {
            fprintf(stderr, "FAIL payload seq=%llu byte=%zu actual=%u expected=%u\n",
                    (unsigned long long)seq, i, p[i], pattern(i, seq)); exit(1);
        }
    }
}
static void completions(urma_jfc_t *cq, unsigned count, uint64_t base, int receive, unsigned size,
                        const urma_jetty_t *jetty, urma_cr_status_t expected_status)
{
    unsigned done = 0; unsigned char seen[MAX_BATCH] = {0};
    struct timespec start, now; CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    while (done < count) {
        urma_cr_t cr[16]; int n = urma_poll_jfc(cq, 16, cr); CHECK(n >= 0);
        for (int i = 0; i < n; ++i) {
            if (cr[i].status != expected_status) {
                fprintf(stderr, "FAIL completion status=%d expected=%d ctx=%llu\n", cr[i].status, expected_status, (unsigned long long)cr[i].user_ctx); exit(1);
            }
            CHECK(cr[i].flag.bs.s_r == (unsigned)receive);
            /* SEND is addressed to a Jetty even when it consumes a shared
             * JFR. The official provider uses local_id to look up that Jetty
             * and then obtains its shared JFR to retire the RQE. */
            if (!cr[i].flag.bs.jetty || cr[i].local_id != jetty->jetty_id.id ||
                cr[i].user_data != (uintptr_t)jetty) {
                fprintf(stderr, "FAIL completion routing jetty_flag=%u local_id=%u expected=%u user_data=%llu expected_pointer=%llu\n",
                        cr[i].flag.bs.jetty, cr[i].local_id, jetty->jetty_id.id,
                        (unsigned long long)cr[i].user_data, (unsigned long long)(uintptr_t)jetty); exit(1);
            }
            CHECK(cr[i].user_ctx >= base && cr[i].user_ctx < base + count);
            unsigned idx = (unsigned)(cr[i].user_ctx - base); CHECK(!seen[idx]); seen[idx] = 1;
            if (receive) CHECK(cr[i].completion_len == size);
            ++done;
        }
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        CHECK(now.tv_sec - start.tv_sec < 60);
    }
    urma_cr_t extra; CHECK(urma_poll_jfc(cq, 1, &extra) == 0);
}
struct exchange {
    uint64_t magic;
    uint32_t size, rounds, batch, opcode, port, depth, jettys, expect_length_error;
    urma_seg_t seg;
    urma_jetty_id_t jetty[MAX_JETTYS];
};
int main(int argc, char **argv)
{
    if (argc != 8 && argc != 9) {
        fprintf(stderr, "Usage: %s DEVICE send|read|write BYTES ROUNDS BATCH DEPTH PORT [SERVER_IP]\n", argv[0]); return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    unsigned size = (unsigned)strtoul(argv[3], NULL, 10), rounds = (unsigned)strtoul(argv[4], NULL, 10);
    unsigned batch = (unsigned)strtoul(argv[5], NULL, 10), depth = (unsigned)strtoul(argv[6], NULL, 10);
    unsigned port = (unsigned)strtoul(argv[7], NULL, 10); int client = argc == 9;
    const char *jettys_env = getenv("UBSIM_INTEGRITY_SHARED_JETTYS");
    unsigned jettys = jettys_env ? (unsigned)strtoul(jettys_env, NULL, 10) : 1;
    CHECK(jettys && jettys <= MAX_JETTYS);
    session_seed = (uint64_t)port * UINT64_C(0xd6e8feb86659fd93);
    int op = !strcmp(argv[2], "send") ? URMA_OPC_SEND : !strcmp(argv[2], "read") ? URMA_OPC_READ : !strcmp(argv[2], "write") ? URMA_OPC_WRITE : -1;
    const char *negative_env = getenv("UBSIM_INTEGRITY_EXPECT_SEND_LENGTH_ERROR");
    int expect_length_error = negative_env && !strcmp(negative_env, "1");
    CHECK(!negative_env || expect_length_error || !strcmp(negative_env, "0"));
    CHECK(!expect_length_error || (op == URMA_OPC_SEND && size == 4097 && rounds == 1 && batch == 1 && jettys == 1));
    CHECK(op >= 0 && size && size <= 1048576 && rounds && rounds <= 1000000);
    CHECK(batch && batch <= MAX_BATCH && depth >= batch && depth <= 8192 && port && port < 65536);
    size_t stride = (size + 2 * GUARD + 63) & ~(size_t)63, length = stride * batch;
    CHECK(length <= 128 * 1024 * 1024);
    uint8_t *buffer; CHECK(posix_memalign((void **)&buffer, 65536, length) == 0); memset(buffer, 0, length);
    urma_init_attr_t init = {0}; CHECK(urma_init(&init) == URMA_SUCCESS);
    urma_device_t *dev = urma_get_device_by_name(argv[1]); CHECK(dev);
    urma_context_t *ctx = urma_create_context(dev, 0); CHECK(ctx);
    urma_jfc_cfg_t cqcfg = {.depth = depth};
    urma_jfc_t *tx = urma_create_jfc(ctx, &cqcfg), *rx = urma_create_jfc(ctx, &cqcfg); CHECK(tx && rx);
    urma_jfr_cfg_t rcfg = {.depth = depth, .trans_mode = URMA_TM_RM, .max_sge = 1, .jfc = rx, .token_value = token, .min_rnr_timer = 12};
    urma_jfr_t *jfr = urma_create_jfr(ctx, &rcfg); CHECK(jfr);
    urma_jetty_cfg_t jcfg = {0}; jcfg.flag.bs.share_jfr = 1;
    jcfg.shared.jfr = jfr; jcfg.shared.jfc = rx;
    jcfg.jfs_cfg = (urma_jfs_cfg_t){.depth = depth, .trans_mode = URMA_TM_RM, .max_sge = 1, .max_rsge = 1, .jfc = tx, .rnr_retry = 6, .err_timeout = 31};
    jcfg.jfs_cfg.flag.bs.multi_path = 1;
    urma_jetty_t *jetty_list[MAX_JETTYS] = {0};
    for (unsigned i = 0; i < jettys; ++i) { jetty_list[i] = urma_create_jetty(ctx, &jcfg); CHECK(jetty_list[i]); }
    urma_token_id_t *tid = urma_alloc_token_id(ctx); CHECK(tid);
    urma_seg_cfg_t scfg = {.va = (uintptr_t)buffer, .len = length, .token_id = tid, .token_value = token};
    scfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;
    scfg.flag.bs.token_id_valid = URMA_TOKEN_ID_VALID;
    urma_target_seg_t *local = urma_register_seg(ctx, &scfg); CHECK(local);
    sock = socket(AF_INET, SOCK_STREAM, 0); CHECK(sock >= 0);
    struct timeval timeout = {.tv_sec = 90};
    CHECK(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (client) {
        CHECK(inet_pton(AF_INET, argv[8], &addr.sin_addr) == 1);
        CHECK(connect(sock, (void *)&addr, sizeof(addr)) == 0);
    } else {
        int one = 1; CHECK(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
        CHECK(bind(sock, (void *)&addr, sizeof(addr)) == 0); CHECK(listen(sock, 1) == 0);
        printf("READY integrity port=%u\n", port);
        int accepted = accept(sock, NULL, NULL); CHECK(accepted >= 0); close(sock); sock = accepted;
        CHECK(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        CHECK(setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    }
    int no_delay = 1; CHECK(setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) == 0);
    struct exchange mine = {.magic = UINT64_C(0x554253494d494e34), .size = size, .rounds = rounds, .batch = batch, .opcode = op, .port = port, .depth = depth, .jettys = jettys, .expect_length_error = expect_length_error, .seg = local->seg}, peer;
    for (unsigned i = 0; i < jettys; ++i) mine.jetty[i] = jetty_list[i]->jetty_id;
    transfer(&mine, sizeof(mine), 1); transfer(&peer, sizeof(peer), 0);
    CHECK(peer.magic == mine.magic && peer.size == size && peer.rounds == rounds && peer.batch == batch && peer.opcode == (unsigned)op);
    CHECK(peer.port == port && peer.depth == depth && peer.jettys == jettys);
    CHECK(peer.expect_length_error == (unsigned)expect_length_error);
    urma_import_seg_flag_t flags = {0}; flags.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;
    urma_target_seg_t *remote = urma_import_seg(ctx, &peer.seg, &token, 0, flags); CHECK(remote);
    urma_target_jetty_t *targets[MAX_JETTYS] = {0};
    for (unsigned i = 0; i < jettys; ++i) {
        urma_rjetty_t rjetty = {.jetty_id = peer.jetty[i], .trans_mode = URMA_TM_RM, .type = URMA_JETTY, .tp_type = URMA_CTP};
        targets[i] = urma_import_jetty(ctx, &rjetty, &token); CHECK(targets[i]);
    }
    for (unsigned round = 0; round < rounds; ++round) {
        /* Rotate only after both directions: every Jetty exercises the same
         * shared JFR/CQs. This checks sequential resource reuse, not concurrent
         * cross-Jetty ordering or independent-JFR isolation. */
        unsigned active = (round / 2) % jettys;
        urma_jetty_t *jetty = jetty_list[active];
        urma_target_jetty_t *target = targets[active];
        uint64_t base = (uint64_t)round * batch + 1;
        /* Alternate initiator each round; independent data per slot and round. */
        int initiator = client == (int)(round % 2);
        int source = op == URMA_OPC_READ ? !initiator : initiator;
        urma_sge_t ls[MAX_BATCH], rs[MAX_BATCH];
        urma_jfs_wr_t wr[MAX_BATCH]; urma_jfr_wr_t rw[MAX_BATCH];
        memset(wr, 0, sizeof(wr)); memset(rw, 0, sizeof(rw));
        for (unsigned i = 0; i < batch; ++i) {
            uint8_t *p = buffer + stride * i + GUARD;
            memset(p - GUARD, 0xa5, size + 2 * GUARD);
            for (unsigned j = 0; j < size; ++j) p[j] = source ? pattern(j, base + i) : 0;
            ls[i] = (urma_sge_t){.addr = (uintptr_t)p, .len = size, .tseg = local};
            rs[i] = (urma_sge_t){.addr = peer.seg.ubva.va + stride * i + GUARD, .len = size, .tseg = remote};
            if (!initiator && op == URMA_OPC_SEND) {
                rw[i].src = (urma_sg_t){.sge = &ls[i], .num_sge = 1}; rw[i].user_ctx = base + i;
                rw[i].next = i + 1 < batch ? &rw[i+1] : NULL;
            }
            wr[i].opcode = op; wr[i].tjetty = target; wr[i].user_ctx = base + i;
            wr[i].flag.bs.complete_enable = 1; wr[i].next = i + 1 < batch ? &wr[i+1] : NULL;
            if (op == URMA_OPC_SEND) wr[i].send.src = (urma_sg_t){.sge = &ls[i], .num_sge = 1};
            else {
                wr[i].rw.src = (urma_sg_t){.sge = op == URMA_OPC_READ ? &rs[i] : &ls[i], .num_sge = 1};
                wr[i].rw.dst = (urma_sg_t){.sge = op == URMA_OPC_READ ? &ls[i] : &rs[i], .num_sge = 1};
            }
        }
        if (!initiator && op == URMA_OPC_SEND) { urma_jfr_wr_t *bad = NULL; CHECK(urma_post_jetty_recv_wr(jetty, rw, &bad) == URMA_SUCCESS); }
        barrier();
        if (initiator) {
            urma_jfs_wr_t *bad = NULL; CHECK(urma_post_jetty_send_wr(jetty, wr, &bad) == URMA_SUCCESS);
            completions(tx, batch, base, 0, size, jetty,
                        expect_length_error ? URMA_CR_LOC_LEN_ERR : URMA_CR_SUCCESS);
        } else if (op == URMA_OPC_SEND && !expect_length_error) completions(rx, batch, base, 1, size, jetty, URMA_CR_SUCCESS);
        barrier(); /* WRITE receiver inspects only after sender completion. */
        for (unsigned i = 0; i < batch; ++i) {
            uint8_t *p = buffer + stride * i + GUARD;
            verify(p, size, base + i, !expect_length_error || initiator);
            if (expect_length_error && !initiator) {
                for (unsigned j = 0; j < size; ++j) CHECK(p[j] == 0);
                urma_cr_t unexpected; CHECK(urma_poll_jfc(rx, 1, &unexpected) == 0);
            }
        }
        barrier(); /* No peer overwrites registered storage while being checked. */
    }
    for (unsigned i = 0; i < jettys; ++i) CHECK(urma_unimport_jetty(targets[i]) == URMA_SUCCESS);
    CHECK(urma_unimport_seg(remote) == URMA_SUCCESS);
    for (unsigned i = 0; i < jettys; ++i) CHECK(urma_delete_jetty(jetty_list[i]) == URMA_SUCCESS);
    CHECK(urma_delete_jfr(jfr) == URMA_SUCCESS);
    CHECK(urma_unregister_seg(local) == URMA_SUCCESS); CHECK(urma_free_token_id(tid) == URMA_SUCCESS);
    CHECK(urma_delete_jfc(tx) == URMA_SUCCESS); CHECK(urma_delete_jfc(rx) == URMA_SUCCESS);
    CHECK(urma_delete_context(ctx) == URMA_SUCCESS); CHECK(urma_uninit() == URMA_SUCCESS);
    free(buffer); close(sock);
    if (expect_length_error)
        printf("PASS negative-send-length bytes=%u expected_status=%d receiver_unchanged=1 role=%s\n", size, URMA_CR_LOC_LEN_ERR, client ? "client" : "server");
    else
        printf("PASS integrity op=%s bytes=%u rounds=%u batch=%u depth=%u shared_jettys=%u schedule=round-robin-pairs payloads=%llu role=%s\n", argv[2], size, rounds, batch, depth, jettys, (unsigned long long)rounds * batch, client ? "client" : "server");
    return 0;
}
