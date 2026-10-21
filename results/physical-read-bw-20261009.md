# User-supplied physical READ bandwidth reference

The accompanying [CSV](physical-read-bw-20261009.csv) transcribes the 26 rows
provided in the conversation on 2026-10-09, including the three extra lengths
outside the `-a23` power-of-two scan. No simulator measurement has been mixed
into this file.

The reported command was `urma_perftest read_bw` in one direction, one Jetty,
one port, with `-a23 -R8192 -T8192 -C524288 -Q1 -l256 -n16384`, CTP/RM, and
`--retry_num 6 --ack_timeout 31 --page_size 65536`. Device name and peer address
in the shared command were examples. The reported path label was
“同节点跨平面-2-单容器”. The user later specified one 400-Gbit/s port and two
nodes connected through one switch, with about 500 ns per switch pass. A
separate physical READ latency curve was supplied in
`results/physical-read-lat-20261009.csv`; run-to-run variance was not supplied.

The user later confirmed `getconf PAGESIZE` returned 65536 on k1, the READ
target. This confirms its kernel base page size, independently of the
perftest option's implementation.

Columns use `mib_s` because the bandwidth and message-rate rows satisfy
`average * 2^20 ≈ size_bytes * message_rate_mpps * 10^6`; the tool's printed
`MB/sec` label is therefore interpreted as MiB/s. The median message rate
for sizes 2 B through 2 KiB is 23.000343 Mpps. The median average bandwidth
for sizes at least 8 KiB is 45602.07 MiB/s = 47.817 GB/s. A 400-Gbit/s port
was subsequently confirmed by the user.

For a provisional 80%-accuracy gate, compare each common size using
`abs(simulated / physical - 1) <= 0.20`. Report coverage separately; matching
only large messages must not count as matching the whole curve. The current
modern gem5 model advertises JFS/JFR 8192 and JFC 524288. The pinned guest
perftest still does not accept `--page_size 65536`; its similarly named
hugepage option is not equivalent. This remaining parameter difference must
be reported with every comparison.
