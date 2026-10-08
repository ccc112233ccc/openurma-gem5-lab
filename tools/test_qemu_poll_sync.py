#!/usr/bin/env python3
"""Compile the real poll callback against deterministic ring/timer stubs.

This tests adapter control flow, not QEMU execution or timing accuracy.
"""
import pathlib
import subprocess
import tempfile
import unittest


class PollSyncTests(unittest.TestCase):
    def test_due_sync_and_output_backpressure(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        source = (root / "integrations/qemu/ubsim-ub-host.c").read_text()
        callback = source.split("static void ubsim_poll(void *opaque)", 1)[1]
        callback = "static void ubsim_poll(void *opaque)" + callback.split(
            "static void ubsim_sync(void *opaque)", 1)[0]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#define QEMU_CLOCK_VIRTUAL 0
#define info_report(...) ((void)0)
union UbSimUbHostD2HMessage { int unused; } message;
typedef struct { int base; } Interface;
typedef struct {
    uint64_t polls, poll_sync_services, poll_ns;
    Interface interface;
    int *poll_timer, *sync_timer, *dummy_timer;
} UbSimUbHostState;
static uint64_t now, next_sync, timestamp;
static int mode, attempts, sent, drains, peeks, poll_timer, sync_timer, dummy;
static bool inbound;
static bool empty_once;
static bool ubsim_sync_active(UbSimUbHostState *s) { return true; }
static uint64_t ubsim_now_ps(UbSimUbHostState *s) { return now; }
static int64_t qemu_clock_get_ns(int clock) { return now; }
static int64_t ubsim_time_from_ps(UbSimUbHostState *s, uint64_t ps) { return ps; }
static void timer_mod(int *timer, int64_t when) { *timer = when; }
static void ubsim_lifecycle_prepare(UbSimUbHostState *s) {}
static void ubsim_lifecycle_commit(UbSimUbHostState *s) {}
static void ubsim_service_device(UbSimUbHostState *s) {
    if (inbound) { ++drains; inbound = false; }
}
static uint64_t UbSimUbHostH2DOutNextSync(Interface *s) { return next_sync; }
static int UbSimUbHostH2DOutSync(Interface *s, uint64_t t) {
    assert(t == now); ++attempts;
    if (mode == 1 && attempts == 1) { inbound = true; return -1; }
    if (mode == 1) assert(drains == 1);
    ++sent; next_sync = now + 10; return 0;
}
static volatile union UbSimUbHostD2HMessage *
UbSimUbHostD2HInPeek(Interface *s, uint64_t limit) {
    assert(++peeks < 10);
    if (empty_once) { empty_once = false; timestamp = now; return NULL; }
    if (inbound) { timestamp = now; return &message; }
    if (sent || mode == 2) { timestamp = now + 5; return &message; }
    timestamp = now; return NULL;
}
static uint64_t UbSimUbHostD2HInTimestamp(Interface *s) { return timestamp; }
static bool SimbricksBaseIfInTerminated(int *base) { return false; }
'''
        main = r'''
int main(void) {
    for (mode = 0; mode < 3; ++mode) {
        now = 100; next_sync = mode == 2 ? 110 : 100;
        attempts = sent = drains = peeks = 0; inbound = false;
        empty_once = mode == 2;
        poll_timer = sync_timer = dummy = 0;
        UbSimUbHostState s = {.poll_timer=&poll_timer,
            .sync_timer=&sync_timer, .dummy_timer=&dummy};
        ubsim_poll(&s);
        assert(poll_timer == 105);
        if (mode != 2) {
            assert(sent == 1 && sync_timer == 110 && s.poll_sync_services == 1);
            assert(attempts == (mode == 1 ? 2 : 1));
        } else assert(sent == 0 && s.poll_sync_services == 0);
        /* The output deadline moved: another call must not duplicate SYNC. */
        empty_once = true;
        ubsim_poll(&s);
        assert(sent == (mode == 2 ? 0 : 1));
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ubsim-poll-test-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(harness + callback + main)
            subprocess.run(["cc", "-std=c11", str(path / "test.c"), "-o",
                            str(path / "test")], check=True, capture_output=True)
            subprocess.run([str(path / "test")], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
