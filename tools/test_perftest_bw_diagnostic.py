"""Compile the actual report functions with a minimal data fixture, without a guest."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "sources/umdk/src/urma/tools/urma_perftest/perftest_run_test.c"
PINNED_COMMIT = "8f272493e4138cd52cfb3ce11064a07c8d1be49f"
SOURCE_REL = "src/urma/tools/urma_perftest/perftest_run_test.c"
PATCH = ROOT / "patches/source/umdk/0011-add-opt-in-bandwidth-report-diagnostic.patch"


class BandwidthDiagnosticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("a C compiler is required")
        cls.tmp = tempfile.TemporaryDirectory(prefix="ubsim-bw-report-test-")
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name)
        # Always reconstruct from the pinned committed input, never the ignored
        # generated working tree. This also works immediately after fetch-sources.
        original = subprocess.run(["git", "show", f"{PINNED_COMMIT}:{SOURCE_REL}"],
                                  cwd=ROOT / "sources/umdk", check=True,
                                  capture_output=True, text=True).stdout
        cls.patched_source = path / "source"
        staged = cls.patched_source / SOURCE_REL
        staged.parent.mkdir(parents=True)
        staged.write_text(original)
        subprocess.run(["git", "apply", str(PATCH)], cwd=cls.patched_source,
                       check=True, capture_output=True, text=True)
        text = staged.read_text()
        start = text.index("static uint64_t calculate_opt_delta(")
        end = text.index("static void print_bi_bw_report(", start)
        fixture = r'''
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define ROUND_UP(n,d) ((((n)+(d)-1)/(d))*(d))
#define PERFTEST_M 1000000
#define PERFTEST_BW_MB 0x100000
#define PERFTEST_MBS 0x100000
#define PERFTEST_SEND 2
#define INF_BI_FACTOR_SEND 1
#define INF_BI_FACTOR_OTHER 2
#define NON_INF_BI_FACTOR 1
#define REPORT_BW_FMT "%u %" PRIu64 " %.9f %.9f %.9f"
typedef struct { uint64_t *tposted, *tcompleted; } run_test_ctx_t;
typedef struct { run_test_ctx_t run_ctx; } perftest_context_t;
typedef struct {
    uint64_t iters, last_iters;
    unsigned jettys, jfs_post_list, cq_mod, size;
    bool no_peak, bidirection, enable_imm;
    int api_type;
    struct { struct { unsigned infinite, iterations, duration; } bs; } time_type;
} perftest_config_t;
typedef struct { unsigned size; uint64_t iters; double bw_peak, bw_avg, msg_rate_avg; } bw_report_data_t;
static unsigned clock_reads;
static uint64_t perftest_get_time(void) { ++clock_reads; return 65000; }
'''
        main = r'''
int main(void) {
    uint64_t posted[32] = {0}, completed[32] = {0};
    posted[0] = 1000; posted[16] = 17000;
    completed[15] = 33000; completed[31] = 65000;
    perftest_context_t ctx = {.run_ctx = {posted, completed}};
    perftest_config_t cfg = {.iters=32, .jettys=1, .jfs_post_list=16, .cq_mod=16,
        .size=4096, .bidirection=true, .api_type=PERFTEST_SEND,
        .time_type={.bs={.iterations=1}}};
    bw_report_data_t report = {0};
    print_bw_report(&ctx, &cfg, &report, posted[0], 1000);
    printf("%.17g %.17g %u\n", report.bw_peak, report.bw_avg, clock_reads);
    return 0;
}
'''
        (path / "fixture.c").write_text(fixture + text[start:end] + main)
        cls.binary = path / "fixture"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(path / "fixture.c"), "-o", str(cls.binary)], check=True,
                       capture_output=True, text=True)

    def run_fixture(self, setting=None):
        env = dict(os.environ)
        env.pop("UBSIM_BW_REPORT_DIAGNOSTIC", None)
        if setting is not None:
            env["UBSIM_BW_REPORT_DIAGNOSTIC"] = setting
        return subprocess.run([str(self.binary)], env=env, check=True,
                              capture_output=True, text=True)

    def test_opt_in_only_and_identical_statistics(self):
        normal = self.run_fixture()
        for setting in ("0", "true", ""):
            result = self.run_fixture(setting)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout, normal.stdout)
        diagnostic = self.run_fixture("1")
        self.assertEqual(normal.stderr, "")
        self.assertEqual(normal.stdout, diagnostic.stdout)
        self.assertIn("observation=existing_samples_only", diagnostic.stderr)
        self.assertEqual(normal.stdout.strip(), "1953.125 1953.125 0")

    def test_full_interval_candidate_and_raw_arithmetic(self):
        result = self.run_fixture("1")
        fields = dict(re.findall(r"(\w+)=([^\s]+)", result.stderr))
        expected = {"tposted0":1000, "array0":1000, "tcompleted0":0,
                    "tcompleted_last":65000, "completed_index":31,
                    "iters_sum":32, "cfg_iters":32, "post_list":16, "cq_mod":16,
                    "opt_delta":2000, "cpu_mhz":1000, "scale":1e9, "cycles_sum":64000}
        for key, value in expected.items():
            self.assertEqual(float(fields[key]), value, key)
        self.assertEqual(float(fields["peak_up"]) / float(fields["peak_down"]),
                         float(fields["peak"]))
        self.assertEqual(float(fields["avg_up"]) / float(fields["avg_down"]),
                         float(fields["avg"]))
        self.assertGreaterEqual(float(fields["peak"]), float(fields["avg"]))

    def test_patch_matches_reconstructed_source(self):
        subprocess.run(["git", "apply", "--reverse", "--check",
                        str(PATCH)], cwd=self.patched_source, check=True,
                       capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
