# Official UB aggregation topology stage

This stage keeps the OLK 6.6 `ubagg.ko`, the UMDK `liburma_ubagg`
provider, `libtpsa`, `ubagg_cli`, the UDMA provider, and `urma_perftest`
unchanged.  The simulator supplies only the device identities and TP-to-port
allocation that real hardware/control firmware would supply.  A small lab
feeder passes the explicit two-node topology to the official
`uvs_set_topo_info()` API in place of the unavailable MXE/scbus source.

## Modeled hardware contract

### Why the stock tools show five device/EID entries

The five visible entries are not five ports and do not imply that gem5 is
simulating two CPU packages.  They are one logical aggregation identity plus
four physical identities required by the stock matrix-server balance
provider:

* one `bonding_dev_0` EID: the application-visible logical aggregation EID;
* plane 0: one primary EID and one physical-port EID;
* plane 1: one primary EID and one physical-port EID.

The official ABI calls the two plane records `io_die_info[2]`.  More
importantly, the unmodified UMDK provider fixes `PRIMARY_EID_NUM` to 2 and
states that matrix-server multipath has two planes.  In `balance` mode it
requires both primary EIDs, creates a physical context/Jetty for each, and
schedules work across them.  This metadata is independent of the number of
gem5 CPU cores.

The provider also supports `standalone`, which consumes only
`io_die_info[0]`.  That path always selects physical path 0 for matrix-server
multipath; it does not turn two port EIDs under one record into balanced
paths.  Therefore collapsing this lab topology to one populated record would
either disable the exercised two-plane balance behavior or require modifying
the official provider, contrary to this stage's design rule.

For a configured 20-bit node EID `B`, `GET_SEID_INFO` exposes four identities:

* primary EIDs: `B`, `B + 0x10000`
* physical-port EIDs: `B + 0x20000`, `B + 0x30000`

The UDMA resource response advertises four EID slots and two ports.  Every
new physical TP receives a unique model TP ID and is assigned to port 0 or 1
at TP creation time in round-robin order.  Packets remain pinned to that
control-plane decision; there is no per-packet hash or benchmark override.

## Reproduction

Boot the normal official-provider dual-node profile with two 400-Gbit/s
ports, the L1-switch topology, and `tp-context` egress selection.  After
`lab sync`, install the same topology on both guests:

```text
node0: ou-ubagg-topology 0 0x100 0x101 0x200 0x201
node1: ou-ubagg-topology 1 0x100 0x101 0x200 0x201
```

Observed with the stock `urma_admin show`:

```text
node0 bonding_dev_0 eid0 ...:0200 ACTIVE
      udma0 eid0/eid1/eid2/eid3 ...:0100/...:0001:0100/
                                      ...:0002:0100/...:0003:0100 ACTIVE
node1 bonding_dev_0 eid0 ...:0201 ACTIVE
      udma0 eid0/eid1/eid2/eid3 ...:0101/...:0001:0101/
                                      ...:0002:0101/...:0003:0101 ACTIVE
```

The official balance provider also opened four physical contexts per guest
and created the corresponding physical JFC/JFR resources.  This proves that
device discovery, the official topology ioctl, aggregation-device creation,
and multi-context provider selection are all executing through the stock
stack.

## Completed in the following stage

The multi-pJetty import/activation path and the receive-completion identity
contract are now implemented by the hardware model.  An end-to-end official
`send_lat --use_bonding --aggr_mode balance` run completes on both nodes and
returns cleanly to both shells.  See `ubagg-dataplane-evidence.md` for the
command, physical-port trace and the exact remaining UMMU limitation.

`urma_admin show --whole` reports `port_count: 2`, but this OLK UDMA driver
fills only `port_attr[0]`; values printed for `port1` are therefore not used
as evidence.  TP allocation/activation and packet traces are the authoritative
port-routing evidence for subsequent stages.
