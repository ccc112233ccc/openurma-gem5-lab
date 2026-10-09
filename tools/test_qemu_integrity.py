import unittest
from tools.run_qemu_integrity import validate_output


class IntegrityOutputTest(unittest.TestCase):
    def test_negative_requires_specific_status_both_roles(self):
        text = "".join(f"--- UART {p} ---\nPASS negative-send-length bytes=4097 expected_status=2 receiver_unchanged=1 role={r}\n"
                       for p, r in ((3560, "server"), (3570, "client")))
        self.assertEqual(validate_output(text, "send", 4097, 1, 1, 512, negative=True), [])
        self.assertTrue(validate_output(text.replace("expected_status=2", "expected_status=1"),
                                        "send", 4097, 1, 1, 512, negative=True))

    def test_exact_endpoint_markers(self):
        lines = [f"PASS integrity op=read bytes=4096 rounds=8 batch=512 depth=512 shared_jettys=1 schedule=round-robin-pairs payloads=4096 role={r}"
                 for r in ("server", "client")]
        text = "--- UART 3560 ---\n" + lines[0] + "\n--- UART 3570 ---\n" + lines[1] + "\n"
        self.assertEqual(validate_output(text, "read", 4096, 8, 512, 512), [])
        for bad in (text.replace("client", "server"), text.replace("4096", "4095"),
                    text + lines[1] + "\n", text.split("--- UART 3570")[0]):
            self.assertTrue(validate_output(bad, "read", 4096, 8, 512, 512))
        concurrent = "--- UART 3560 ---\n" + (lines[0]+"\n")*2 + "--- UART 3570 ---\n" + (lines[1]+"\n")*2
        self.assertEqual(validate_output(concurrent, "read", 4096, 8, 512, 512, True), [])
