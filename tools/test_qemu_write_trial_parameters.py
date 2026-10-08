import unittest
from run_qemu_timing_trial import write_bw_command, validate_write_capabilities


class WriteTrialParametersTest(unittest.TestCase):
    def test_existing_control_unchanged(self):
        self.assertEqual(write_bw_command(1048576, 21410),
            "LD_LIBRARY_PATH=/lib:/usr/lib urma_perftest write_bw "
            "-d udma0 --eid_idx 0 --ctp -B -s 1048576 -P 21410 "
            "-J 1 -I 64 -T 64 -n 64 -l 16 -Q 16 -p 0")

    def test_512_depth_and_longer_run(self):
        self.assertIn("-I 64 -T 512 -n 16384 -l 256 -Q 1",
                      write_bw_command(4096, 21411, 16384, 512, 256, 1))

    def test_inline_is_independent(self):
        self.assertIn("-I 0 -T 512", write_bw_command(4096, 21411, depth=512, inline_size=0))
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


if __name__ == "__main__":
    unittest.main()
