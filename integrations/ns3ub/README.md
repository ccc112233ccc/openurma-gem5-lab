# ns-3-UB external adapter integration

This directory contains the complete lab-owned source for the process boundary
between gem5 OpenURMA endpoints and the upstream ns-3-UB fabric:

- `ub-gem5-adapter.cc` is the standalone ns-3 process.
- `ub-external-adapter-protocol.h` is its shared-memory ring ABI.
- `register-adapter-header.patch` registers the ABI header with the upstream
  ns-3 module build.

The upstream ns-3-UB checkout is a generated dependency under
`sources/ns-3-ub` at the revision recorded in `SOURCE_REVISIONS.md`. During
`./lab build ns3ub`, the two checked-in files are installed into that checkout,
the small CMake patch is applied, and `scratch_ub-gem5-adapter` is built.

Keeping the complete adapter here makes the integration reviewable from this
repository. It also avoids depending on unpublished commits in a sibling
checkout. The adapter files retain their GPL-2.0-only SPDX declaration.
