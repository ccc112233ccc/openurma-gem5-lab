"""A newly saved checkpoint must remain restorable on the same gem5 build."""

import tempfile
from pathlib import Path
from unittest import TestCase

from tools.normalize_gem5_checkpoint import normalize


class NormalizeGem5CheckpointTest(TestCase):
    def test_flash_lock_bool_vector_is_rewritten_once(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "m5.cpt"
            path.write_text("[other]\nlocked=0 1\n"
                            "[system.realview.flash1.blocks]\nlocked=0 1 0\n"
                            "other=0 1\n")
            self.assertTrue(normalize(path))
            self.assertEqual("[other]\nlocked=0 1\n"
                             "[system.realview.flash1.blocks]\n"
                             "locked=false true false\nother=0 1\n",
                             path.read_text())
            self.assertFalse(normalize(path))
