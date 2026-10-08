# CTP reliability profile correction and official READ512 acceptance

## Protocol basis, not an RTO fit

UB Base Specification2.0, Chinese PDF page160, section6.3.2 describes a CTP
implementation relying on lower layers for reliable delivery, without CTP
transport acknowledgements or retransmission. It is an implementation example,
not a prohibition on every possible extension. Pages213–215, section7.3.1.1
separately require per-slice transaction responses and retries for recoverable
transaction errors such as RNR/Page Fault; these are not a blanket instruction
to replay every unacknowledged TA on a periodic timeout. Section6.4.2's
transport timeout machinery concerns RTP, not this CTP implementation.

Official download entry:
https://www.unifiedbus.com/zh?callback=download&fileName=UB-Base-Specification-2.0-zh.pdf&fileType=docs

The locally reviewed PDF SHA256 is
`ebed6871cd4681c49d672a2548067358dc23f1d97d258fe64277b912b9a458eb`.
The main agent read the relevant pages directly; independent agents reviewed
protocol meaning, source and counterexamples. System pdftotext/pypdf were
unavailable; the bundled Python pypdf reader succeeded without installation.

Pinned ns-3-UB CTP did not implement the ordinary TA timeout-replay timer.
It was added by our patch0001. The adapter already defaulted this extension
off, but both launchers enabled it whenever virtual-time synchronization was
enabled. That coupling had no protocol basis.

## Implementation

- Both launchers now resolve `UBSIM_CTP_RETRANSMISSION=auto` to off, independently
  of synchronization. Explicit on/off remain available and recorded in manifests.
- Runtime labels distinguish `ctp-lossless-lower-layer-assumed` from
  `experimental-ta-timeout-replay`. The former assumes lossless delivery; it is
  **not** proof of a complete link retry/CRC/reconnect implementation.
- The bandwidth checker verifies runtime profile against the effective manifest
  when a runtime label exists. Historical unknown profiles remain unknown.
- The native READ harness preserves its historical default on, adds explicit
  off and response-drop controls, and records the selected profile/configuration.
  A changed-profile pass cannot overwrite an existing evidence directory.
- Corrected earlier documentation: `UbJettyInflightMax` is512 in-flight WQE
  segments per native Jetty; the CTP context ACK bitmap capacity is2048
  (`UB_JETTY_TASSN_OOO_THRESHOLD`). The observed512 high-water mark is neither
  context capacity nor proof of512 active WQEs.

Official UMDK `perftest_resources.c:1945–1947` copies retry_num/ack_timeout into
user-TP configuration; the call is gated by `enable_user_tp` at2107. The tested
CTP commands do not set that flag. No mapping of those arguments to this native
timer is established. Do not silently treat them as CTP retry controls.

No RTO, switch policy, link rate/delay, CPU timing or guest binary was changed.
The old experimental-on failure is retained, not relabelled as repaired.

## Native acceptance and negative controls

`experiments/native-ctp-profiles-20261009` uses400G,64 KiB WQEs and unchanged
25.6 us/7-retry settings (inactive when off).

| Case | Completed/submitted | Result | Retries | Whole wall(s) |
| --- | --- | --- | ---: | ---: |
| off-n512 |1024/1024|PASS|0|0.804|
| on-n512 |968/1024|FAIL,56 errors|18216|2.178|
| off-drop-n1 |0/2|FAIL,missing responses|0|0.513|
| on-drop-n1 |2/2|PASS,recovered|2|0.539|

The independent drop tests drop one response per endpoint. Off reaches the
peer's10 ms simulated deadline with15/16 returned TAs per WQE and exits nonzero;
it produces neither a successful completion nor a protocol error completion.
On recovers with exactly16 target executions per endpoint, no duplicate DMA.
Neither process termination nor `work_drained=null` is called successful drain.
The on-n512 run overlapped the guest trial on the host; its wall time is not a
performance comparison with off-n512.

## Official full-system synchronized READ result

Evidence: `experiments/qemu-read-64k-ctp-lower-layer-20261009`.
Two QEMU TCG guests, standalone UDMA processes and ns-3; conservative sync is
required, effective retry extension off. Both guests use the normal official
guest image. No gem5 cold boot.

```sh
urma_perftest read_bw -d udma0 --eid_idx 0 --ctp -B \
  -s 65536 -n 2048 -T 512 -R 512 -J 1 -l 16 -Q 16 -I 0 -P 21410 -p 0
# client appends: -S 10.0.0.1
```

Check the recorded command in report.json for the full invocation. Official
output: peak88789.87 MiB/s, average88252.58 MiB/s, MsgRate1.412041 Mpps.
Average conversion is **92.53953732608 GB/s**, already combining both endpoints
because the command uses `-B`; it is not a single400G direction's throughput.

4096/4096 WQEs complete, application bytes268435456,65536 READ response TAs;
zero retries, packet drops, failed WQEs or target DMA errors. Each endpoint
observes RMA pending peak496 and two SQ consumer wraps. The64 KiB size is within
both official drivers' advertised maximum. Post-workload two-sample live-work
drain succeeds, but is not whole-system checkpoint quiescence.

Shell-ready46.447 s; workload83.248 s; drain0.262 s; total135.173 s
(command135.21 s). All simulation processes stopped after evidence collection.
The read-only checker passes, including runtime/manifest profile agreement.
CQ moderation/large-message accuracy and ubcore cleanup warnings remain in
the logs; UDMA unknown_queue_writes is1183 per endpoint. Zero errors above
refers to the checked data-path counters, not all MMIO or all warning categories.
This is successful official-driver READ512 configured-depth acceptance for
this lossless profile, not complete fault recovery or proof of512 active WQEs.

## Tests and remaining work

Adapter rebuild2.504 s. Policy tests cover both launchers, sync on/off and
auto/on/off; syntax checks pass. The final48 policy/CLI/bandwidth-audit unit tests
pass in0.19 s. Earlier47-test batch including manifest checks passed in0.35 s.

Continue small-size READ bandwidth sweeps and parameter sensitivity, then
multi-Jetty/port contention. Preserve explicit-extension failure coverage.
RNR/Page Fault recovery and complete LLR are not established by the lossless
result. Manual experiments with actual loss can still wait indefinitely;
automated experiments must retain an external deadline and report failure,
never synthesize CQ success or claim drain after killing processes.
