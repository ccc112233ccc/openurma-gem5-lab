import unittest
from tools.check_udma_profile import check_profile


class ProfileTests(unittest.TestCase):
    good = ("[UDMA_PROFILE] eid=0x100 sq_wqes=2048 sq_completions=2048 "
            "sq_depth_rejects=0 sq_decode_rejects=0 ubase_errors=0 "
            "rma_pending_peak=32 sq_wqebbs_retired=2048 sq_consumer_wraps=2")

    def test_accepts_observed_concurrency(self):
        self.assertEqual(check_profile(self.good, 2048)[1], [])

    def test_rejects_serialized_incomplete_or_missing_observation(self):
        for text in ("", self.good + "\n" + self.good,
                     self.good.replace("rma_pending_peak=32", "rma_pending_peak=1"),
                     self.good.replace("sq_completions=2048", "sq_completions=2047"),
                     self.good.replace("ubase_errors=0", "ubase_errors=1"),
                     self.good.replace("sq_consumer_wraps=2", "")):
            self.assertTrue(check_profile(text, 2048)[1])
