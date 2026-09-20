# Live dual-terminal evidence (2026-09-20)

These files were captured directly from two macOS Terminal windows attached to
the two gem5 PL011 consoles:

- node0: `localhost:3460`, guest `openurma-node0`, OOB `10.0.0.1`
- node1: `localhost:3470`, guest `openurma-node1`, OOB `10.0.0.2`

The guests use the `fast` profile (one `AtomicSimpleCPU` per guest), the stock
official UDMA provider and kernel drivers, two 400-Gbit/s UB ports, and the
explicit L1-switch topology.  The topology was installed through
`ou-ubagg-topology`; `urma_admin show` then reported `bonding_dev_0` as
`ACTIVE` on both guests.

The clean screenshots show the following collective test.  Node0 omitted
`-S`; node1 appended `-S 10.0.0.1`.

```sh
OPENURMA_DIST_SYNC=1 urma_perftest read_lat \
  -d bonding_dev_0 --eid_idx 0 --ctp --use_bonding \
  --aggr_mode balance -s 128 -P 21122 -J 1 -I 0 \
  -l 1 -n 8 -p 0
```

Both processes returned zero.  The client reported two measured samples after
five synchronized warm-up deltas: min 2.78 us, max 2.98 us, median 2.86 us,
average 2.88 us.

`05-read-lat-success.mov` is a real screen recording limited to the rectangle
containing the two Terminal windows.  It records the preceding successful run
on port 21121.  Files `08-*` and `09-*` are the clearest static evidence: they
show the command summary, benchmark output, return code, and guest prompts in
the same terminal windows.
