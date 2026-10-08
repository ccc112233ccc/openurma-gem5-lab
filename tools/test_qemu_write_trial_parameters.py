import unittest
from run_qemu_timing_trial import (write_bw_command, validate_write_capabilities,
                                   bw_command, validate_bw_capabilities, parse_args)


class WriteTrialParametersTest(unittest.TestCase):
    def test_opt_in_report_diagnostic(self):
        base = bw_command("send_bw", 4096, 21410)
        self.assertEqual(bw_command("send_bw", 4096, 21410, report_diagnostic=True),
                         "UBSIM_BW_REPORT_DIAGNOSTIC=1 " + base)
        self.assertTrue(parse_args(["unused", "--report-diagnostic"]).report_diagnostic)
        with self.assertRaises(SystemExit):
            parse_args(["unused", "--report-diagnostic", "--user-read-scan"])

    def test_default_sizes_stay_within_advertised_message_capacity(self):
        self.assertEqual(parse_args(["unused-output"]).sizes, [4096, 65536])
        caps = "".join(f"--- UART {port} ---\nmax_jfs_depth : 1024\n"
                       "max_jfr_depth : 1024\nmax_msg_size : 65536\n"
                       "max_jfs_inline_size : 256\n" for port in (3560, 3570))
        for verb in ("write_bw", "read_bw"):
            self.assertEqual(len(validate_bw_capabilities(caps, verb, 512, 64, 65536)), 2)
            with self.assertRaises(ValueError):
                validate_bw_capabilities(caps, verb, 512, 64, 1048576)

    def test_existing_control_unchanged(self):
        self.assertEqual(write_bw_command(1048576, 21410),
            "LD_LIBRARY_PATH=/lib:/usr/lib urma_perftest write_bw "
            "-d udma0 --eid_idx 0 --ctp -B -s 1048576 -P 21410 "
            "-J 1 -I 64 -T 64 -n 64 -l 16 -Q 16 -p 0")

    def test_512_depth_and_longer_run(self):
        self.assertIn("-I 64 -T 512 -n 16384 -l 256 -Q 1",
                      write_bw_command(4096, 21411, 16384, 512, 256, 1))

    def test_inline_is_independent(self):
        self.assertIn("-I 0 -T 512", write_bw_command(4096, 21411, iterations=512,
                                                    depth=512, inline_size=0))
        with self.assertRaises(ValueError):
            write_bw_command(4096, 21411, inline_size=-1)

    def test_incompatible_arguments_rejected(self):
        for settings in ((0, 64, 16, 16), (64, 8, 16, 1),
                         (64, 8, 1, 16), (65, 64, 16, 1), (64, 64, 1, 3)):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                write_bw_command(4096, 21410, *settings)

    def test_reported_capabilities(self):
        caps = "".join(f"--- UART {port} ---\nmax_jfs_depth : 1024\n"
                       "max_jfs_inline_size : 256\n" for port in (3560, 3570))
        self.assertEqual(len(validate_write_capabilities(caps, 512, 64)), 2)
        for text, depth, inline in ((caps, 512, 512), (caps, 2048, 64),
                                   (caps.replace("max_jfs_depth", "unknown"), 512, 64),
                                   (caps.split("--- UART 3570")[0], 512, 64)):
            with self.subTest(depth=depth, inline=inline), self.assertRaises(ValueError):
                validate_write_capabilities(text, depth, inline)

    def test_read_and_send_requested_trials(self):
        read = bw_command("read_bw", 1048576, 21410, 2048, 512, 16, 16)
        self.assertIn("read_bw -d udma0 --eid_idx 0 --ctp -B", read)
        self.assertIn("-I 0 -T 512 -R 512 -n 2048 -l 16 -Q 16", read)
        send = bw_command("send_bw", 4096, 21411, 16384, 512, 16, 16)
        self.assertIn("send_bw -d udma0 --eid_idx 0 --ctp -B", send)
        self.assertIn("-I 64 -T 512 -R 512 -n 16384", send)
        with self.assertRaises(ValueError):
            bw_command("send_bw", 4097, 21411)
        for settings in ((64, 512, 16, 16), (64, 64, 16, 32)):
            with self.assertRaises(ValueError):
                bw_command("read_bw", 4096, 21410, *settings)

    def test_receive_and_message_capability_validation(self):
        caps = "".join(f"--- UART {port} ---\nmax_jfs_depth : 1024\n"
                       "max_jfr_depth : 512\nmax_msg_size : 1048576\n"
                       "max_jfs_inline_size : 0\n" for port in (3560, 3570))
        self.assertEqual(len(validate_bw_capabilities(caps, "read_bw", 512, 64, 1048576)), 2)
        for text, verb, depth, size in (
                (caps, "send_bw", 512, 4096),  # Inline 64 exceeds reported zero.
                (caps, "read_bw", 1024, 4096),
                (caps, "read_bw", 512, 2097152),
                (caps.replace("max_jfr_depth", "unknown"), "read_bw", 512, 4096),
                (caps + "max_jfr_depth : 512\n", "read_bw", 512, 4096)):
            with self.subTest(verb=verb, depth=depth, size=size), self.assertRaises(ValueError):
                validate_bw_capabilities(text, verb, depth, 64, size)

    def test_cli_aliases_and_duplicate_sizes(self):
        common = ["unused-output", "--sizes", "4096", "65536"]
        generic = parse_args(common + ["--iterations", "2048", "--depth", "512",
                                       "--post-list", "16", "--cq-mod", "16", "--inline", "0"])
        legacy = parse_args(common + ["--write-iterations", "2048", "--write-depth", "512",
                                      "--write-post-list", "16", "--write-cq-mod", "16", "--write-inline", "0"])
        self.assertEqual(vars(generic), vars(legacy))
        self.assertEqual(parse_args(["unused", "--user-read-scan"]).user_read_scan, True)
        for options in (["--sizes", "4096", "4096"],
                        ["--user-read-scan", "--read-sizes", "2", "2"],
                        ["--bw-verb", "send_bw", "--sizes", "8192"]):
            with self.subTest(options=options), self.assertRaises(SystemExit):
                parse_args(["unused"] + options)


if __name__ == "__main__":
    unittest.main()
