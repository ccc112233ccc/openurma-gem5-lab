#!/usr/bin/env python3
"""Parser contracts only; synthetic fixture does not validate a simulator."""
import json
from pathlib import Path
import tempfile
import unittest
from check_qemu_write_trial import audit, AuditError


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        command = "urma_perftest write_bw -B -s 4096 -n 64 -T 512 -J 1 -l 16 -Q 16 -I 64"
        records = [dict(label=label, returncode=0) for label in ("launch", "shell-ready", "network", "stop")]
        records += [dict(label="write-4096", returncode=0, validation_errors=[],
                         command=["--commands", command, command + " -S 10.0.0.1"])]
        self.write("report.json", json.dumps(dict(records=records, wall_seconds=10)))
        self.write("run-manifest.txt", "runtime=qemu-tcg-timing\nnode_count=2\nnetwork_backend=modular-ns3ub\nsynchronization=required\nuart0=3560\nuart1=3570\npeer_link_rate_gbps=400\n")
        body = ""
        for port in (3560, 3570):
            body += (f"--- UART {port} ---\nURMA_WRITE Bidirectional JFS Post List BandWidth Test\n"
                     "JFS depth : 512\nMax inline size : 64[B]\nBW peak[MB/sec] BW average[MB/sec]\n"
                     f"4096 64 1100.00 1000.00 0.100\n__UBSIM_RC_{port}_123__=0\n")
        self.write("write-4096.txt", body)
        self.write("ns3.log", "[NS3_UB_NET_PROGRESS] native_tasks=0 pending_target_dma=0 scheduled_packets=0 outputs_empty=1 native_wqes_submitted=128 native_wqes_completed=128 native_wqes_failed=0\n"
                   "[NS3_UB_NET_STATS] native_wqes_submitted=128 native_wqes_completed=128 payload_bytes=524288 ctp_request_segments=128 ctp_taacks=128 ctp_native_completions=128 native_wqes_failed=0 target_dma_errors=0 pending_target_dma=0 ctp_read_responses=0 ns3_runtime_drops=0 native_trace_size_mismatches=0 ctp_retransmission_exhausted=0 async_timestamp_jumps=0\n")
        for node in (0, 1):
            self.write(f"udma-node{node}-udma.log", "[UDMA_PROFILE] sq_wqes=64 sq_completions=64 sq_depth_rejects=0 sq_decode_rejects=0 ubase_errors=0 async_timestamp_jumps=0 rma_pending_peak=17 sq_consumer_wraps=2\n"
                       "[UB_HOST_PENDING] reads=0 writes=0\n"
                       "[UDMA_PROGRESS] sq_wqes=64 sq_completions=64 quiescent=1 pending_rma=0 read_dma_queues=0 remote_services=0 completion_events=0 event_busy=0 receive_busy=0 destroy_pending=0\n")

    def write(self, name, value):
        (self.root / name).write_text(value)

    def replace(self, name, old, new):
        self.write(name, (self.root / name).read_text().replace(old, new))

    def test_success_units_and_do_not_add_exchanged_rows(self):
        result = audit(self.root)
        self.assertEqual(result["cases"][0]["average_GB_s"], 1.048576)
        self.assertNotEqual(result["cases"][0]["average_GB_s"], 2.097152)
        self.assertEqual(result["nodes"][0]["observed_rma_pending_peak"], 17)
        self.assertEqual(result["nodes"][0]["observed_sq_consumer_wraps"], 2)

    def test_missing_log(self):
        (self.root / "ns3.log").unlink()
        with self.assertRaises(AuditError): audit(self.root)

    def test_wrong_counts(self):
        self.replace("ns3.log", "native_wqes_completed=128", "native_wqes_completed=64")
        with self.assertRaises(AuditError): audit(self.root)

    def test_unknown_units(self):
        self.replace("write-4096.txt", "MB/sec", "GB/s")
        with self.assertRaises(AuditError): audit(self.root)

    def test_endpoint_already_summed_mismatch(self):
        text = (self.root / "write-4096.txt").read_text()
        self.write("write-4096.txt", text.replace("1100.00 1000.00", "2200.00 2000.00", 1))
        with self.assertRaises(AuditError): audit(self.root)

    def test_drain_cannot_replace_failed_command(self):
        self.replace("write-4096.txt", "__=0", "__=255")
        with self.assertRaises(AuditError): audit(self.root)

    def test_missing_peak_not_zero(self):
        self.replace("udma-node0-udma.log", "rma_pending_peak=17", "")
        with self.assertRaises(AuditError): audit(self.root)

    def test_no_sync_rejected(self):
        self.replace("run-manifest.txt", "synchronization=required", "synchronization=disabled")
        with self.assertRaises(AuditError): audit(self.root)

    def test_busy_host_dma_rejected(self):
        self.replace("udma-node0-udma.log", "reads=0", "reads=1")
        with self.assertRaises(AuditError): audit(self.root)

    def test_read_mixture_rejected(self):
        report = json.loads((self.root / "report.json").read_text())
        report["records"].append(dict(label="read-4096", returncode=0))
        self.write("report.json", json.dumps(report))
        with self.assertRaises(AuditError): audit(self.root)

    def test_depth_not_inline_or_observed_peak(self):
        self.replace("write-4096.txt", "JFS depth : 512", "JFS depth : 64")
        with self.assertRaises(AuditError): audit(self.root)

    def test_missing_link_configuration(self):
        self.replace("run-manifest.txt", "peer_link_rate_gbps=400", "")
        with self.assertRaises(AuditError): audit(self.root)

    def test_missing_inline_command(self):
        self.replace("report.json", " -I 64", "")
        with self.assertRaises(AuditError): audit(self.root)

    def test_missing_inline_header(self):
        self.replace("write-4096.txt", "Max inline size : 64[B]", "")
        with self.assertRaises(AuditError): audit(self.root)

    def test_different_endpoint_inline(self):
        report = json.loads((self.root / "report.json").read_text())
        report["records"][-1]["command"][-1] = report["records"][-1]["command"][-1].replace("-I 64", "-I 128")
        self.write("report.json", json.dumps(report))
        with self.assertRaises(AuditError): audit(self.root)

    def test_zero_inline_valid_and_distinct_from_depth(self):
        self.replace("report.json", "-I 64", "-I 0")
        self.replace("write-4096.txt", "64[B]", "0[B]")
        result = audit(self.root)
        self.assertEqual(result["cases"][0]["inline_size"], 0)
        self.assertEqual(result["cases"][0]["configured_sq_depth"], 512)

    def test_negative_inline_rejected(self):
        self.replace("report.json", "-I 64", "-I -1")
        with self.assertRaises(AuditError): audit(self.root)


if __name__ == "__main__":
    unittest.main()
