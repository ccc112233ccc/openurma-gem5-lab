#!/usr/bin/env python3
"""Build an isolated ARM64 perftest with statement-level READ BW timing."""

from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "artifacts/perftest-loop-profile"
SOURCE = ROOT / "sources/umdk/src/urma/tools/urma_perftest/perftest_run_test.c"
TARGET = ROOT / "artifacts/umdk-build/urma/tools/urma_perftest"


def one(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"expected one source marker, found {text.count(old)}: {old[:90]!r}")
    return text.replace(old, new)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    raw = SOURCE.read_text()
    begin = raw.index("static int run_once_bw(")
    end = raw.index("static int clean_scq_credit(", begin)
    body = raw[begin:end]
    body = one(body, "    while (tot_scnt < tot_iters || tot_ccnt < tot_iters ||\n"
               "        (cfg->time_type.bs.duration == 1 && run_ctx->state != END_STATE)) {\n",
               "    while (tot_scnt < tot_iters || tot_ccnt < tot_iters ||\n"
               "        (cfg->time_type.bs.duration == 1 && run_ctx->state != END_STATE)) {\n"
               "        uint64_t loop_begin = loop_profile_on ? loop_ticks() : 0;\n"
               "        uint64_t loop_post_ticks = 0;\n"
               "        if (loop_profile_on) loop_profile.outer_loops++;\n")
    body = one(body, "                urma_jfs_wr_t *bad_wr = NULL;\n"
               "                if (cfg->jetty_mode == PERFTEST_JETTY_SIMPLEX) {",
               "                urma_jfs_wr_t *bad_wr = NULL;\n"
               "                uint64_t post_begin = loop_profile_on ? loop_ticks() : 0;\n"
               "                if (cfg->jetty_mode == PERFTEST_JETTY_SIMPLEX) {")
    body = one(body, "                        &bad_wr);\n                }\n\n"
               "                if (status != URMA_SUCCESS) {",
               "                        &bad_wr);\n                }\n"
               "                if (loop_profile_on) {\n"
               "                    uint64_t delta = loop_ticks() - post_begin;\n"
               "                    loop_profile.post_ticks += delta;\n"
               "                    loop_post_ticks += delta;\n"
               "                    loop_profile.post_calls++;\n"
               "                }\n\n"
               "                if (status != URMA_SUCCESS) {")
    body = one(body, "            }\n        }\n\n"
               "        if (tot_ccnt < tot_iters || (cfg->time_type.bs.duration == 1 && tot_ccnt < tot_scnt)) {",
               "            }\n        }\n"
               "        if (loop_profile_on) {\n"
               "            loop_profile.scan_ticks += loop_ticks() - loop_begin - loop_post_ticks;\n"
               "            loop_profile.jetty_visits += cfg->jettys;\n"
               "        }\n\n"
               "        if (tot_ccnt < tot_iters || (cfg->time_type.bs.duration == 1 && tot_ccnt < tot_scnt)) {")
    body = one(body, "            cqe_cnt = urma_poll_jfc(ctx->jfc_s[0], PERFTEST_POLL_BATCH, cr);\n"
               "            if (cqe_cnt > 0) {",
               "            uint64_t poll_begin = loop_profile_on ? loop_ticks() : 0;\n"
               "            cqe_cnt = urma_poll_jfc(ctx->jfc_s[0], PERFTEST_POLL_BATCH, cr);\n"
               "            uint64_t poll_end = loop_profile_on ? loop_ticks() : 0;\n"
               "            if (loop_profile_on) {\n"
               "                loop_profile.poll_ticks += poll_end - poll_begin;\n"
               "                loop_profile.poll_calls++;\n"
               "                if (cqe_cnt > 0) loop_profile.cqes += cqe_cnt;\n"
               "                if (cqe_cnt == 0) loop_profile.empty_polls++;\n"
               "            }\n"
               "            if (cqe_cnt > 0) {")
    body = one(body, "                goto free_cr;\n            }\n        }\n    }\n\n"
               "    if (cfg->no_peak == true && cfg->time_type.bs.iterations == 1) {",
               "                goto free_cr;\n            }\n"
               "            if (loop_profile_on) loop_profile.account_ticks += loop_ticks() - poll_end;\n"
               "        }\n    }\n\n"
               "    if (cfg->no_peak == true && cfg->time_type.bs.iterations == 1) {")
    insert = r'''
#if defined(__aarch64__)
static inline uint64_t loop_ticks(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}
static inline uint64_t loop_frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}
#else
#include <time.h>
static inline uint64_t loop_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static inline uint64_t loop_frequency(void) { return 1000000000ull; }
#endif

static const bool loop_profile_on = false;
'''
    # The flag is evaluated once per run; the global state is otherwise inert.
    insert = insert.replace("static const bool loop_profile_on = false;", r'''
static struct {
    uint64_t outer_loops, jetty_visits, post_calls, poll_calls;
    uint64_t cqes, empty_polls;
    uint64_t scan_ticks, post_ticks, poll_ticks, account_ticks;
} loop_profile;
static bool loop_profile_on;

__attribute__((destructor)) static void emit_loop_profile(void)
{
    if (!loop_profile_on) return;
    fprintf(stderr,
        "[URMA_LOOP] frequency_hz=%llu outer_loops=%llu jetty_visits=%llu "
        "post_calls=%llu poll_calls=%llu cqes=%llu empty_polls=%llu "
        "scan_ticks=%llu post_ticks=%llu poll_ticks=%llu account_ticks=%llu\n",
        (unsigned long long)loop_frequency(),
        (unsigned long long)loop_profile.outer_loops,
        (unsigned long long)loop_profile.jetty_visits,
        (unsigned long long)loop_profile.post_calls,
        (unsigned long long)loop_profile.poll_calls,
        (unsigned long long)loop_profile.cqes,
        (unsigned long long)loop_profile.empty_polls,
        (unsigned long long)loop_profile.scan_ticks,
        (unsigned long long)loop_profile.post_ticks,
        (unsigned long long)loop_profile.poll_ticks,
        (unsigned long long)loop_profile.account_ticks);
}
''')
    body = one(body, "    uint64_t tot_scnt = 0;", "    loop_profile_on = getenv(\"UBSIM_BW_LOOP_PROFILE\") != NULL;\n"
               "    uint64_t tot_scnt = 0;")
    output = one(raw, raw[begin:end], body)
    output = one(output, "#define PERFTEST_HALF (2)", insert + "\n#define PERFTEST_HALF (2)")
    source = BUILD / "perftest_run_test.c"
    source.write_text(output)
    header = subprocess.run(["git", "show", "fd4d120^:tools/ou-m5ops.h"],
                            cwd=ROOT, check=True, capture_output=True).stdout
    (BUILD / "ou-m5ops.h").write_bytes(header)
    cmake = TARGET / "CMakeFiles/urma_perftest.dir"
    values = {}
    for line in (cmake / "flags.make").read_text().splitlines():
        if " = " in line:
            key, value = line.split(" = ", 1)
            values[key] = shlex.split(value)
    flags = values["C_DEFINES"] + values["C_INCLUDES"] + values["C_FLAGS"]
    stale = "/workspace/openurma-gem5-lab/sources/OpenURMA/integration/umdk/vendor/umdk"
    flags = [flag.replace(stale, str(ROOT / "sources/umdk")) for flag in flags]
    flags += [f"-I{BUILD}", f"-I{SOURCE.parent}"]
    obj = BUILD / "perftest_run_test.c.o"
    subprocess.run(["gcc", *flags, "-c", str(source), "-o", str(obj)], check=True)
    link = shlex.split((cmake / "link.txt").read_text())
    link[link.index("CMakeFiles/urma_perftest.dir/perftest_run_test.c.o")] = str(obj)
    binary = BUILD / "urma_perftest_loop_profile"
    link[link.index("-o") + 1] = str(binary)
    subprocess.run(link, cwd=TARGET, check=True)
    print(binary, binary.stat().st_size)


if __name__ == "__main__":
    main()
