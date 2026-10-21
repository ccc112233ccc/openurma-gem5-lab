# CTP MTU and small READ latency check (2026-10-12)

## Question and protocol conclusion

The current ns3ub CTP model uses `UB_MTU_BYTE = 4096` as a **maximum
transaction payload per segment**, not a fixed wire allocation. The native
WQE segmenter takes `min(4096, bytes_left)`; a READ request carries zero
data payload but describes the requested byte count; the READ response
carries exactly that count. CTP tail padding is only to a four-byte boundary
(0–3 bytes). `UbPort::TransmitPacket` serializes `packet->GetSize()` at its
400-Gbit/s data rate. References:

- `sources/ns-3-ub/src/unified-bus/model/ub-datatype.h`: `UB_MTU_BYTE`,
  `GetNextPacketSize`, `PeekNextPacketSize`.
- `sources/ns-3-ub/src/unified-bus/model/protocol/ub-ctp.cc`:
  `BuildCtpPacket` and its CTP padding.
- `sources/ns-3-ub/src/unified-bus/model/ub-port.cc`:
  `TransmitPacket` and `CalculateBytesTxTime(packet->GetSize())`.
- `integrations/ns3ub/ub-net-adapter.cc`: exact READ segment response length.

The native `ctp-read-length-contract` passed all three modes, including
1–129 B and 4095/4096/4097/4098/8193 B. It checks exact target range,
response length, and completion. This is a native contract, not a full guest
timing result.

The physical port's *active* MTU has not been reported. The public
[URMA API Guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html)
defines both `active_mtu` and `max_mtu`, and lists 256 through 8192 B MTU
enumerations. Thus 4 KiB is confirmed for **this model**, not for the
physical port. The user's bandwidth data rule out 4-KiB padding of every
small READ response: at 128 B and 23.098464 million READs/s, a 4096-B
response each time would require 756.9 Gbit/s before any headers, above the
one-port 400-Gbit/s ceiling.

## Full-system latency sweep

Restored `shell-ready-modern-ddr5-jetty47-perjetty-20261010` through
`./lab --runtime docker start-ready --profile modern --provider official
--network-backend modular-ns3ub --roi-sync --ub-port-count 1`, then ran
`./lab --runtime docker sync`. The two guests used the official CTP/RM
provider, one Jetty, one 400-Gbit/s UB port, 500 ns per switch pass, and
synchronized ArmO3 timing. Each size used a separate `read_lat -s SIZE`
command with `-I 128 -T 8192 -R 8192 -C 524288 -n 4096 -Q 1 -p 0`.
Each pair exited zero and completed its five-part lifecycle fence. Perftest
removed six warm-up samples, leaving 4090 samples per size. The original
physical sweep used 16,384 iterations and did not explicitly set queue
depths; the guest printed JFS depth 4096 and JFC depth 524288. These numbers
check the *shape* but are not a fully matched absolute latency calibration.

| Sequence | READ bytes | Physical mean (µs) | gem5 mean (µs) | Physical p99 (µs) | gem5 p99 (µs) | Host wall (s) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2 | 3.38 | 2.51 | 3.52 | 4.65 | 170.24 |
| 2 | 128 | 3.38 | 2.41 | 3.52 | 4.57 | 175.22 |
| 3 | 1024 | 3.54 | 2.47 | 3.64 | 4.61 | 187.38 |
| 4 | 4096 | 3.75 | 2.68 | 3.90 | 4.89 | 210.45 |
| 5 | 2 repeat | 3.38 | 2.42 | 3.52 | 4.57 | 191.34 |

Total measured host wall: **934.62 s**. Full results, UART transcripts,
per-case fence logs and machine manifest are in
`experiments/gem5-read-lat-mtu-docker-20261012/`. The driver is
`tools/run_gem5_read_latency_sweep.py`. All simulators were stopped after
the sweep. The initial 2-B mean exceeds its warmed repeat by 0.09 µs;
comparison of sizes should use the repeat as its baseline.

The warmed simulated 2/128-B means are nearly flat at 2.42/2.41 µs. The
1024-B point rises to 2.47 µs and the 4096-B point to 2.68 µs. The physical
2/128-B means are 3.38/3.38 µs, then 3.54/3.75 µs. The model therefore
has the intended fixed-cost-dominated shape without 4-KiB padding, but the
1024/4096-B increases above 128 B are only 0.06/0.27 µs versus physical
0.16/0.37 µs. Current mean predictions are 26–30% lower than their physical
reference, failing the provisional ±20% target. The 4090-sample p99 values
are also higher than the corresponding physical p99 values. These
differences do not identify a unique endpoint component to retune; the
500-ns switch pass should not be inflated to hide endpoint error.

At 400 Gbit/s, 1024 B and 4096 B of payload serialize in 20.48 ns and
81.92 ns per link transmission, respectively. The nearly flat small-size
latency is consistent with fixed request/response, device, DMA, memory and
software costs dominating this byte-serialization contribution. It is not
evidence of a 4-KiB-sized physical response.
