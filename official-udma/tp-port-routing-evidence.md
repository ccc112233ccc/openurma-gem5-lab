# Official TP-context physical-port routing

This checkpoint connects the unmodified official UDMA control/data contract to
the simulated physical ports. Software creates and activates the TP; the
device model executes the resulting TP-to-port route. No official OLK driver,
UMDK core, `urma_perftest`, or `liburma-udma.so` source file was modified.

## Configuration

```sh
OPENURMA_TRACE_PACKETS=1 \
bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh \
  --profile fast --provider official --ub-port-count 2 \
  --peer-topology l1-switch --peer-port-map 0,1 \
  --peer-port-selection tp-context

bash /Users/caobo/workspace/openurma-gem5-lab/sync-dual.sh
bash /Users/caobo/workspace/openurma-gem5-lab/run-latency.sh \
  --profile ctp-rm-send-imm-i128 --samples 5 --size 128 --port 21115
```

The model reports the selected mechanism explicitly:

```text
[NIC peer topology] mode=l1-switch port_selection=tp-context port_map=0->0,1->1
```

## SEND: SQE TPN selects the programmed TP route

The official control path allocated and activated one TP on each node:

```text
node0: allocated TP id=94 tpn=94 port=0 source=GET_TP_LIST
node0: activated TP id=94 tpn=94 count=1 port=0
node1: allocated TP id=1009 tpn=1009 port=0 source=GET_TP_LIST
node1: activated TP id=1009 tpn=1009 count=1 port=0
```

Packet traces then showed the same TPN in every decoded official SEND SQE:

```text
node0: src_port=0 dst_port=0 tpn=94 op=0x81 len=128
node1: src_port=0 dst_port=0 tpn=1009 op=0x81 len=128
```

Five measured iterations plus synchronized warm-up completed on both nodes.
Both reported 0.53 us average and returned zero. No `no active TP route`
diagnostic appeared.

## WRITE: all fragments and ACKs remain on the selected port

A second stock test used the official bidirectional WRITE path:

```sh
# node 0, start first
OPENURMA_DIST_SYNC=1 urma_perftest write_bw -d udma0 --eid_idx 0 \
  --ctp -B -s 8192 -P 21252 -J 1 -I 64 -n 5 -l 1 -Q 1 -p 0
# node 1
OPENURMA_DIST_SYNC=1 urma_perftest write_bw -d udma0 -S 10.0.0.1 \
  --eid_idx 0 --ctp -B -s 8192 -P 21252 -J 1 -I 64 \
  -n 5 -l 1 -Q 1 -p 0
```

This allocation selected the other physical port:

```text
node0: allocated TP id=232 tpn=232 port=1; activated on port=1
node1: allocated TP id=426 tpn=426 port=1; activated on port=1
```

Each 8-KiB WRITE was split into the existing peer-ring maximum payloads of
8088 and 104 bytes. Every fragment used `src_port=1,dst_port=1`; the target
generated opcode `0x83` ACKs on the request's ingress port 1. Both endpoints
completed five iterations, reported the same 17,898.14 MiB/s average, and
returned zero.

## Scope

This proves a single-port TP programmed by the official control path, including
port preservation for fragmented WRITE and the return path. It does not claim
one-TP multi-port striping. That requires modeling the official bonding-group
table and the hardware hash inside the configured group rather than extending
the old simulator-only object-field hash.
