import unittest

from build_perftest_bw_diagnostic import replace_perftest


def entry(name, data=b"", mode=0o100755):
    fields = [123, mode, 17, 19, 1, 123456, len(data), 0, 0, 0, 0,
              len(name.encode()) + 1, 0]
    record = b"070701" + b"".join(f"{v:08x}".encode() for v in fields)
    record += name.encode() + b"\0"
    record += b"\0" * (-len(record) % 4)
    record += data
    return record + b"\0" * (-len(record) % 4)


class DiagnosticArchiveTest(unittest.TestCase):
    def test_only_target_record_changes(self):
        before = entry("etc/config", b"configdata")
        after = entry("usr/lib/provider.so", b"providercontents")
        trailer = entry("TRAILER!!!")
        raw = before + entry("./usr/bin/urma_perftest", b"old") + after + trailer
        output, replaced, unchanged = replace_perftest(raw, b"new-binary-payload")
        expected = before + entry("./usr/bin/urma_perftest", b"new-binary-payload") + after + trailer
        self.assertEqual(output, expected + b"\0" * (-len(expected) % 512))
        self.assertEqual(len(replaced), 1)
        self.assertEqual(unchanged, 3)

    def test_missing_or_duplicate_target_rejected(self):
        trailer = entry("TRAILER!!!")
        duplicate = entry("usr/bin/urma_perftest", b"one") + entry("./usr/bin/urma_perftest", b"two")
        for raw in (trailer, duplicate + trailer):
            with self.assertRaises(RuntimeError):
                replace_perftest(raw, b"new")


if __name__ == "__main__":
    unittest.main()
