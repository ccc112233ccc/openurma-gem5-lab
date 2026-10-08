#!/usr/bin/env python3
"""Parser contracts only; synthetic fixture does not validate a simulator."""
import json
from pathlib import Path
import tempfile
import unittest
from check_qemu_write_trial import audit, AuditError, peak_policy, check_report_diagnostic, command_config


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        command = "urma_perftest write_bw --ctp -d udma0 --eid_idx 0 -B -s 4096 -n 64 -T 512 -J 1 -l 16 -Q 16 -I 64"
        records = [dict(label=label, returncode=0) for label in ("launch", "shell-ready", "network", "stop")]
        records += [dict(label="write-4096", returncode=0, validation_errors=[],
                         command=["--commands", command, command + " -S 10.0.0.1"])]
        self.write("report.json", json.dumps(dict(records=records, wall_seconds=10)))
        self.write("run-manifest.txt", "runtime=qemu-tcg-timing\nnode_count=2\nnetwork_backend=modular-ns3ub\nsynchronization=required\nuart0=3560\nuart1=3570\npeer_link_rate_gbps=400\n")
        body = ""
        for port in (3560, 3570):
            body += (f"--- UART {port} ---\nURMA_WRITE Bidirectional JFS Post List BandWidth Test\n"
                     "JFS depth : 512\nNumber of jettys : 1\nCQ Moderation : 16\nJFS post list : 16\nMax inline size : 64[B]\nBW peak[MB/sec] BW average[MB/sec]\n"
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

    def set_verb(self, verb, correct_counters=True):
        self.replace("report.json", "write", verb)
        self.replace("report.json", "-T 512", "-T 512 -R 512")
        raw = (self.root / "write-4096.txt").read_text().replace("URMA_WRITE", f"URMA_{verb.upper()}")
        if verb == "read":
            self.replace("report.json", "-I 64", "-I 0")
            raw = raw.replace("Max inline size : 64[B]\n", "")
        self.write(f"{verb}-4096.txt", raw)
        if correct_counters:
            self.replace("ns3.log", "ctp_taacks=128", "ctp_taacks=0")
            if verb == "read":
                self.replace("ns3.log", "ctp_read_responses=0", "ctp_read_responses=128 ctp_native_send_delivery_completions=0")
                self.replace("ns3.log", "payload_bytes=524288", "payload_bytes=1048576")
            else:
                self.replace("ns3.log", "ctp_read_responses=0", "ctp_read_responses=0 ctp_native_send_delivery_completions=128")

    def test_read_accounting_doubles_diagnostic_not_bandwidth(self):
        self.set_verb("read")
        result = audit(self.root, "read")
        self.assertEqual(result["cases"][0]["average_GB_s"], 1.048576)
        self.assertEqual(result["nodes"][0]["observed_rma_pending_peak"], 17)

    def test_read_cannot_pass_write_counters(self):
        self.set_verb("read", False)
        with self.assertRaises(AuditError): audit(self.root)

    def test_read_requires_response_bytes_accounting(self):
        self.set_verb("read")
        self.replace("ns3.log", "payload_bytes=1048576", "payload_bytes=524288")
        with self.assertRaises(AuditError): audit(self.root)

    def test_send_has_no_rma_outstanding_claim(self):
        self.set_verb("send")
        result = audit(self.root)
        self.assertNotIn("observed_rma_pending_peak", result["nodes"][0])
        self.assertIsNone(result["nodes"][0]["send_outstanding_peak"])
        self.assertEqual(result["verb"], "send")

    def test_send_cannot_pass_write_counters(self):
        self.set_verb("send", False)
        with self.assertRaises(AuditError): audit(self.root)

    def test_send_requires_delivery_completion(self):
        self.set_verb("send")
        self.replace("ns3.log", "ctp_native_send_delivery_completions=128", "ctp_native_send_delivery_completions=0")
        with self.assertRaises(AuditError): audit(self.root)

    def test_explicit_verb_mismatch_rejected(self):
        with self.assertRaises(AuditError): audit(self.root, "read")

    def test_read_nonzero_inline_rejected(self):
        self.set_verb("read")
        self.replace("report.json", "-I 0", "-I 64")
        with self.assertRaises(AuditError): audit(self.root)

    def test_read_send_require_receive_depth(self):
        for verb in ("read", "send"):
            with self.subTest(verb=verb):
                from check_qemu_write_trial import command_config
                command = f"urma_perftest {verb}_bw --ctp -d udma0 --eid_idx 0 -B -s 64 -n 64 -T 512 -J 1 -l 16 -Q 16 -I 0"
                with self.assertRaises(AuditError): command_config(command)

    def test_command_identity_and_roles(self):
        original = (self.root / "report.json").read_text()
        for old, new in (("--ctp", ""), ("-d udma0", "-d wrong"), ("--eid_idx 0", "--eid_idx 1"),
                         ("-S 10.0.0.1", "-S 10.0.0.2"), ("-S 10.0.0.1", "")):
            with self.subTest(new=new):
                self.write("report.json", original.replace(old,new))
                with self.assertRaises(AuditError): audit(self.root)

    def test_server_cannot_also_be_client(self):
        report = json.loads((self.root / "report.json").read_text())
        report["records"][-1]["command"][-2] += " -S 10.0.0.1"
        self.write("report.json", json.dumps(report))
        with self.assertRaises(AuditError): audit(self.root)

    def test_endpoint_receive_depth_mismatch(self):
        self.set_verb("send")
        report = json.loads((self.root / "report.json").read_text())
        report["records"][-1]["command"][-1] = report["records"][-1]["command"][-1].replace("-R 512", "-R 64")
        self.write("report.json", json.dumps(report))
        with self.assertRaises(AuditError): audit(self.root)

    def test_raw_parameter_mismatch(self):
        original = (self.root / "write-4096.txt").read_text()
        for old,new in (("CQ Moderation : 16", "CQ Moderation : 1"),
                        ("Number of jettys : 1", "Number of jettys : 2"),
                        ("JFS post list : 16", "JFS post list : 1")):
            with self.subTest(old=old):
                self.write("write-4096.txt", original.replace(old,new))
                with self.assertRaises(AuditError): audit(self.root)

    def test_message_rate_finite_nonnegative_and_equal(self):
        original = (self.root / "write-4096.txt").read_text()
        for value in ("nan", "inf", "-1", "0.200"):
            with self.subTest(value=value):
                self.write("write-4096.txt", original.replace("0.100",value,1))
                with self.assertRaises(AuditError): audit(self.root)

    def capabilities(self, first=65536, second=65536):
        self.write("capabilities.txt", "".join(
            f"--- UART {port} ---\nmax_msg_size : {size}\n__UBSIM_RC_{port}_123__=0\n"
            for port, size in ((3560, first), (3570, second))))

    def test_capabilities_absent_is_unknown_not_supported(self):
        result = audit(self.root)
        self.assertEqual(result["status"], "PASS")
        self.assertIsNone(result["advertised_size_compliance"])

    def test_within_both_advertised_limits(self):
        self.capabilities()
        self.assertTrue(audit(self.root)["advertised_size_compliance"])

    def test_above_one_limit_keeps_counter_fact_not_support_claim(self):
        self.capabilities(second=2048)
        result = audit(self.root)
        self.assertEqual(result["status"], "PASS")
        self.assertFalse(result["advertised_size_compliance"])
        self.assertTrue(result["capability_warnings"])
        self.assertEqual(result["cases"][0]["average_GB_s"], 1.048576)

    def test_incomplete_capabilities_do_not_prove_support(self):
        self.capabilities()
        self.replace("capabilities.txt", "max_msg_size : 65536", "unknown : 65536")
        result = audit(self.root)
        self.assertIsNone(result["advertised_size_compliance"])
        self.assertTrue(result["capability_warnings"])

    def test_source_peak_rules(self):
        self.assertEqual(peak_policy("send",20000)[0], [1,0])
        for verb in ("send","write","read"):
            self.assertEqual(peak_policy(verb,20001)[0], [1,1])
        self.assertEqual(peak_policy("write",20000)[0], [0,0])

    def test_send_aggregate_avg_may_exceed_client_peak_without_diagnostic(self):
        self.set_verb("send")
        self.replace("send-4096.txt", "1100.00 1000.00", "900.00 1000.00")
        result=audit(self.root)
        self.assertEqual(result["cases"][0]["endpoint_no_peak"], [1,0])

    def test_write_enabled_peak_bound_preserved(self):
        self.replace("write-4096.txt", "1100.00 1000.00", "900.00 1000.00")
        with self.assertRaises(AuditError): audit(self.root)

    def test_unsupported_report_modes_rejected(self):
        original=(self.root/"report.json").read_text()
        for flag in ("--enable_imm", "-N", "-D 5", "--run_infinitely", "--unknown"):
            self.write("report.json", original.replace("--ctp", "--ctp "+flag))
            with self.subTest(flag=flag), self.assertRaises(AuditError): audit(self.root)

    @staticmethod
    def diagnostic(no_peak):
        opt=64000 if no_peak else 1000
        fields=dict(observation="existing_samples_only", api_type=2,bidirection=1,no_peak=no_peak,
                    tposted0=100,array0=100,tcompleted0=64100 if no_peak else 1100,
                    tcompleted_last=64100,completed_index=0 if no_peak else 63,
                    iters_sum=64,cfg_iters=64,jettys=1,post_list=16,cq_mod=16,size=4096,
                    opt_delta=opt,cpu_mhz=1,scale=1000000,cycles_sum=64000,
                    peak_up=0 if no_peak else 4096000000,peak_down=opt*1048576,
                    avg_up=262144000000,avg_down=67108864000,peak=0 if no_peak else 3.90625,avg=3.90625)
        return "[UBSIM_BW_REPORT_DIAGNOSTIC] "+" ".join(f"{k}={v}" for k,v in fields.items())

    def add_diagnostics(self):
        self.set_verb("send")
        self.replace("send-4096.txt", "1100.00 1000.00 0.100", "3.91 7.81 0.002000")
        raw=(self.root/"send-4096.txt").read_text()
        for port, np in ((3560,1),(3570,0)):
            raw=raw.replace(f"__UBSIM_RC_{port}_123__=0", self.diagnostic(np)+f"\n__UBSIM_RC_{port}_123__=0")
        self.write("send-4096.txt",raw)

    def test_diagnostic_endpoint_arithmetic_and_summed_rounding(self):
        self.add_diagnostics()
        self.assertTrue(audit(self.root)["cases"][0]["report_diagnostics_checked"])

    def test_diagnostic_bad_index_scale_arithmetic_or_sum_rejected(self):
        self.add_diagnostics()
        original=(self.root/"send-4096.txt").read_text()
        for old,new in (("completed_index=0","completed_index=63"), ("no_peak=1","no_peak=0"),
                        ("cpu_mhz=1","cpu_mhz=2"), ("avg_up=262144000000","avg_up=1"),
                        ("cycles_sum=64000","cycles_sum=1"), ("3.91 7.81","3.91 8.00")):
            self.write("send-4096.txt",original.replace(old,new))
            with self.subTest(old=old), self.assertRaises(AuditError): audit(self.root)

    def test_one_sided_diagnostic_rejected(self):
        self.add_diagnostics()
        self.replace("send-4096.txt",self.diagnostic(1),"")
        with self.assertRaises(AuditError): audit(self.root)


if __name__ == "__main__":
    unittest.main()
