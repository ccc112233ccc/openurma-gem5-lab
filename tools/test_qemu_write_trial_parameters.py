import unittest
from run_qemu_timing_trial import write_bw_command


class WriteTrialParametersTest(unittest.TestCase):
    def test_existing_control_unchanged(self):
        self.assertEqual(write_bw_command(1048576, 21410),
            "LD_LIBRARY_PATH=/lib:/usr/lib urma_perftest write_bw "
            "-d udma0 --eid_idx 0 --ctp -B -s 1048576 -P 21410 "
            "-J 1 -I 64 -T 64 -n 64 -l 16 -Q 16 -p 0")

    def test_512_depth_and_longer_run(self):
        self.assertIn("-I 512 -T 512 -n 16384 -l 256 -Q 1",
                      write_bw_command(4096, 21411, 16384, 512, 256, 1))

    def test_incompatible_arguments_rejected(self):
        for settings in ((0, 64, 16, 16), (64, 8, 16, 1),
                         (64, 8, 1, 16), (65, 64, 16, 1), (64, 64, 1, 3)):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                write_bw_command(4096, 21410, *settings)


if __name__ == "__main__":
    unittest.main()
