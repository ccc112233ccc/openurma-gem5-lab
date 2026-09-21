# Dynamic EID routing checkpoint

Date: 2026-09-21

## Result

Four independent gem5 full-system guests now attach to one independent UB
switch process and one shared Ethernet control switch. Any guest can select any
other guest with the server's `10.0.0.(node+1)` address. The IP is used only by
the official `urma_perftest` TCP resource exchange; UB DATA and synchronization
records are routed by the peer EID learned through the official TP setup path.

This replaces the earlier fixed `0<->1`, `2<->3` endpoint map. The historical
pair-scaling measurement remains in `results/sync-nnode-20260921/`.

## Driver-to-model path

No destination-selection shortcut was added to the official UDMA provider or
OLK UDMA driver:

1. `urma_perftest -S IP` connects to the selected remote process and exchanges
   the normal UMDK resources, including EID.
2. The official OLK UDMA `GET_TP_LIST` control request already contains the
   local and peer EIDs. The modeled UDMA control queue records those values
   against the TPN returned to the driver.
3. The official provider writes that 24-bit TPN into every SQE.
4. The modeled device reads the SQE TPN, resolves its peer EID, and emits a
   version-3 64-byte Adapter header containing source and destination EIDs.
5. `ub-switch-sim` validates the source and forwards the record to the endpoint
   registered for the destination EID. READ responses and WRITE ACKs carry the
   request source EID back through the same routing table.

The EID low 16 bits identify the endpoint. This also lets the endpoint's
per-port alias EIDs route to the same modeled device while physical-port
selection remains the NIC/TP model's responsibility.

## Synchronization

Each endpoint publishes the active session's destination EID together with its
local ON/OFF generation. The switch synchronizes only reciprocal peers; an
unrelated guest does not enter that dependency set. Local generation numbers
need not match, which is important when a node finishes one session and later
talks to a different node.

Fine-grained synchronization starts after TCP resource exchange and TP setup.
Before that point no destination EID exists, so setup runs outside the measured
epoch. The synchronized warm-up and measurement loop retains the 100 ns
lookahead and virtual receive timestamps.

## Shared control network

The old per-pair Ethernet relays were replaced by one N-port learning switch.
It learns source MAC addresses, unicasts known destinations, and floods
broadcast, multicast and unknown-unicast frames. Nodes have unique addresses:

| node | UART | EID suffix | OOB address |
| ---: | ---: | ---: | --- |
| 0 | 3460 | `0100` | `10.0.0.1` |
| 1 | 3470 | `0101` | `10.0.0.2` |
| 2 | 3480 | `0102` | `10.0.0.3` |
| 3 | 3490 | `0103` | `10.0.0.4` |

## Reproduction

```bash
./run-dual.sh --nodes 4 --profile fast --provider official \
  --sync-mode adapter-local
./sync-dual.sh

# node3 client -> node0 server
./run-node-pair-latency.sh 0 3 20 128 22203

# then change node0's peer: node0 client -> node2 server
./run-node-pair-latency.sh 2 0 20 128 22220
```

Manual commands follow the same rule: run the server without `-S`, then use
`-S 10.0.0.(server+1)` on the chosen client. The peer EID is not supplied as a
simulator-only command-line argument.

## Validation evidence

- node3 -> node0 CTP/RM/SEND_IMM, 128 bytes, 20 measured samples: both sides
  completed with 1.06 us average and 1.07 us p99.
- node0 -> node2 immediately afterward: both sides again completed with
  1.06 us average and 1.07 us p99, proving peer changes do not require equal
  synchronization generation counters.
- node2 <-> node1 and node0 <-> node3 completed concurrently, proving two
  independent EID sessions can share one switch.
- node3 -> node1 official `write_lat` completed, exercising WRITE request and
  destination-routed ACK.
- node1 -> node2 official `read_lat` completed, exercising READ request and
  destination-routed response.
- `tools/test_ub_switch_sim.py` covers cross-former-pair DATA routing, dynamic
  synchronization, invalid/loopback protection and unequal phase generations.
- `tools/test_ethernet_relay.py` covers broadcast flooding, MAC learning and
  known-destination unicast on three ports.

These are functional and virtual-time-model checks, not a calibration against
a physical switch latency.
