# Result index

Only consolidated, reviewable reports are kept in Git. Raw UART transcripts,
gem5 output trees and sweep directories are reproducible runtime artifacts and
are intentionally ignored.

- `eid-routing-20260921/REPORT.md` — current arbitrary-peer EID routing,
  shared OOB switch and SEND/READ/WRITE validation.
- `sync-nnode-20260921/REPORT.md` — historical four-node fixed-pair scaling
  checkpoint.
- `sync-cpp-ab-20260921/REPORT.md` — moving Adapter synchronization from
  Python stepping into the gem5 C++ event queue.
- `sync-ab-20260921/REPORT.md` — original Adapter-local versus global-barrier
  comparison.

The removed raw records remain available in repository history through commit
`dc1c3ff`. New retained experiments should add one report with the exact
configuration, aggregate result and reproduction command rather than checking
in an entire simulator output directory.
