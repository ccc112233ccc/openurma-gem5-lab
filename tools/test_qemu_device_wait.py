#!/usr/bin/env python3
"""Exercise actual patched QEMU CPU gates with deterministic pending-IRQ stubs.

Requires the QEMU source tree prepared by the UBSim builder. This is a
control-flow regression, not a substitute for a full-system timing test.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


class DeviceWaitTests(unittest.TestCase):
    def test_irq_does_not_resume_pending_device_transaction(self):
        root = Path(__file__).resolve().parents[1]
        qemu = Path(os.environ.get("UBSIM_TEST_QEMU_SOURCE",
                                   root / "sources/qemu-11.1.1"))
        cpus = (qemu / "system/cpus.c").read_text()
        execution = (qemu / "accel/tcg/cpu-exec.c").read_text()
        overlay = (root / "integrations/qemu/ubsim-tcg-sync.c").read_text()
        functions = "\n".join((
            function(cpus, "bool cpu_thread_is_idle(CPUState *cpu)"),
            function(cpus, "bool cpu_can_run(CPUState *cpu)"),
            function(execution, "static inline bool cpu_handle_halt(CPUState *cpu)"),
            function(overlay, "void ubsim_tcg_set_retaddr(uintptr_t retaddr)"),
            function(overlay, "G_NORETURN void ubsim_tcg_suspend_cpu(CPUState *cpu)"),
            function(overlay, "void ubsim_tcg_resume_cpu(CPUState *cpu)"),
        ))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#define g_assert assert
#define G_NORETURN _Noreturn
#define EXCP_HALTED 17
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
typedef struct CPUState CPUState;
typedef struct { bool (*cpu_exec_halt)(CPUState *); } TCGCPUOps;
typedef struct { const TCGCPUOps *tcg_ops; } CPUClass;
struct CPUState {
    bool stop, stopped, work_pending, halted, ubsim_device_wait;
    int exception_index;
    unsigned interrupt_request;
    CPUClass *cc;
};
static unsigned halt_calls, restore_calls, direct_calls;
static uintptr_t ubsim_tcg_retaddr;
static jmp_buf escaped;
static bool cpu_work_list_empty(CPUState *cpu) { return !cpu->work_pending; }
static bool cpu_is_stopped(CPUState *cpu) { return cpu->stopped; }
/* A masked but asserted IRQ still wakes ordinary ARM WFI. */
static bool cpu_has_work(CPUState *cpu) { return cpu->interrupt_request != 0; }
static bool ordinary_halt(CPUState *cpu) {
    ++halt_calls;
    return cpu_has_work(cpu);
}
static struct { bool (*cpu_thread_is_idle)(CPUState *); } accel;
static __typeof__(accel) *cpus_accel = &accel;
static G_NORETURN void cpu_loop_exit_restore(CPUState *cpu, uintptr_t retaddr) {
    assert(retaddr == 0x1234); ++restore_calls; longjmp(escaped, 1);
}
static G_NORETURN void cpu_loop_exit(CPUState *cpu) {
    ++direct_calls; longjmp(escaped, 1);
}
'''
        checks = r'''
int main(void) {
    TCGCPUOps ops = {.cpu_exec_halt = ordinary_halt};
    CPUClass klass = {.tcg_ops = &ops};
    /* Static storage keeps values defined across longjmp. */
    static CPUState cpu;
    cpu.cc = &klass;
    cpu.interrupt_request = 0x42;
    for (unsigned restored = 0; restored != 2; ++restored) {
        ubsim_tcg_set_retaddr(restored ? 0x1234 : 0);
        if (setjmp(escaped) == 0) ubsim_tcg_suspend_cpu(&cpu);
        assert(cpu.ubsim_device_wait && cpu.halted);
        assert(cpu.exception_index == EXCP_HALTED);
        assert(cpu.interrupt_request == 0x42);
        for (unsigned retry = 0; retry != 4; ++retry) {
            assert(!cpu_can_run(&cpu));
            assert(cpu_thread_is_idle(&cpu));
            unsigned previous = halt_calls;
            assert(cpu_handle_halt(&cpu));
            assert(halt_calls == previous && cpu.halted);
            assert(cpu.interrupt_request == 0x42);
        }
        cpu.stop = true;
        assert(!cpu_thread_is_idle(&cpu) && !cpu_can_run(&cpu));
        cpu.stop = false;
        cpu.work_pending = true;
        assert(!cpu_thread_is_idle(&cpu));
        cpu.work_pending = false;
        cpu.stopped = true;
        assert(cpu_thread_is_idle(&cpu) && !cpu_can_run(&cpu));
        cpu.stopped = false;
        ubsim_tcg_resume_cpu(&cpu);
        assert(!cpu.ubsim_device_wait && !cpu.halted);
        assert(cpu.interrupt_request == 0x42);
        assert(cpu_can_run(&cpu) && !cpu_thread_is_idle(&cpu));
        cpu.halted = true;
        assert(!cpu_thread_is_idle(&cpu));
        unsigned previous = halt_calls;
        assert(!cpu_handle_halt(&cpu));
        assert(halt_calls == previous + 1 && !cpu.halted);
    }
    assert(restore_calls == 1 && direct_calls == 1);
    cpu.interrupt_request = 0;
    cpu.halted = true;
    assert(cpu_thread_is_idle(&cpu) && cpu_handle_halt(&cpu));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ubsim-device-wait-") as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness + functions + checks)
            subprocess.run(["cc", "-std=gnu11", str(path / "test.c"),
                            "-o", str(path / "test")], check=True,
                           capture_output=True, timeout=30)
            subprocess.run([str(path / "test")], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
