"""Port allocation contracts for multi-host QEMU runs."""

import unittest
from unittest.mock import patch

from ubsim_lab.qemu_ports import qemu_ports


class QemuPortsTests(unittest.TestCase):
    def test_two_node_ports_keep_existing_layout(self):
        with patch.dict("os.environ", {}, clear=True):
            self.assertEqual(qemu_ports(2),
                             {"uart": [3560, 3570], "ssh": [2220, 2221],
                              "oob": [4560]})

    def test_eight_node_ports_are_unique(self):
        with patch.dict("os.environ", {}, clear=True):
            ports = qemu_ports(8)
        self.assertEqual(ports["uart"][-1], 3630)
        self.assertEqual(ports["ssh"][-1], 2227)
        self.assertEqual(ports["oob"][-1], 4567)
        self.assertEqual(len(set(sum(ports.values(), []))), 24)

    def test_collision_is_rejected_before_launch(self):
        with patch.dict("os.environ", {"UBSIM_QEMU_OOB_PORT": "3560"}):
            with self.assertRaisesRegex(ValueError, "unique"):
                qemu_ports(3)


if __name__ == "__main__":
    unittest.main()
