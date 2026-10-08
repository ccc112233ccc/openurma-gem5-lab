# Opt-in bandwidth report diagnosis

The diagnostic is an uninstalled overlay, not part of the pinned UMDK bundle
or the regular guest image. The normal source pin remains
`8f272493e4138cd52cfb3ce11064a07c8d1be49f`. Merely fetching sources or setting
the environment variable in the existing guest does not install this feature.

`patches/source/umdk/0011-add-opt-in-bandwidth-report-diagnostic.patch` adds an
optional `UBSIM_BW_REPORT_DIAGNOSTIC=1` stderr line to `print_bw_report`. It
observes existing arrays, iteration counts, selected minimum delta, conversion
scale, and raw peak/average numerators and denominators. It adds no clock reads
and changes neither reporting formulas nor driver behavior. The diagnostic is
not an extra measurement and does not fix or normalize an anomalous result.

## Reproduce without changing the checkout or guest

After the normal source fetch, from the lab root:

```sh
python3 -m unittest discover -s tools -p test_perftest_bw_diagnostic.py
```

The test uses `git show` at the pinned commit, writes that one source file into
a temporary tree, and applies the maintained patch there. It does not rely on
an ignored modified `sources/umdk` working tree. It compiles the actual report
functions with a minimal data fixture, checks opt-in behavior, confirms zero
extra clock reads and unchanged output, and verifies full-interval arithmetic.
Three tests passed in 0.40 seconds including native compilation. These are
reporting-function tests, not guest or official-provider acceptance.

Minimal manual application to a clean pinned UMDK checkout, from the lab root:

```sh
git -C sources/umdk apply --check ../../patches/source/umdk/0011-add-opt-in-bandwidth-report-diagnostic.patch
git -C sources/umdk apply ../../patches/source/umdk/0011-add-opt-in-bandwidth-report-diagnostic.patch
```

If the overlay is already present, use `git apply --reverse --check` against
the same patch to verify it instead of applying twice. The patch does not
modify the stock provider subtree. It must be explicitly applied or integrated
into a future source bundle before building a guest that exposes the switch.

## Isolated ARM build and packaging, October 9 follow-up

The current regular image still contains the unchanged ELF with SHA256
`7724a94b9e10970bf18a40f45be11a36e19d316fec4a9b0d0f7a0444de9fa84e`.
The earlier preparation phase did not start a build. A later authorized
follow-up started the existing `ubsim-gem5-lab` ARM64 Linux container and built
an isolated diagnostic with GCC 11.4. No regular image or ELF was replaced.

`tools/build_perftest_bw_diagnostic.py` reconstructs the pinned source in an
independent Git root, applies 0011, compiles one object and links the diagnostic
ELF against the other four unchanged perftest objects. Source and ELF diagnostic
markers are both checked before packaging. The independent Git root is required:
an initial attempt inside the main repository's ignored artifacts directory
caused `git apply` to skip paths without adding the diagnostic. That attempt was
caught by inspecting the ELF and was retained as an unpatched control, not used
as a diagnostic image.

Successful compile/link wall time was **0.604 seconds**; reconstruction, build,
and packaging totaled **4.544 seconds**. The script replaces exactly the
`usr/bin/urma_perftest` newc record in the original image; all other **121 records**
are copied byte-for-byte, including the official provider and drivers.

- Diagnostic image: `out/official-udma-bw-diagnostic.cpio.gz`.
- Image SHA256: `e0e40c5e2b002a155807c8c48605543eaf43322f392d2cf04f52c95ff24f0476`.
- Diagnostic ELF SHA256: `96dcc28ab9e562a3b535e854ff1ae2e29a3d447e1899dc56eb8e5d7803441f65`.
- Full build commands and provenance: `out/official-udma-bw-diagnostic.manifest.json`.

The ELF extracted independently with `tar -xOf` matches the recorded hash. The
unpatched rebuild and regular ELF have identical `.text` byte dumps (excluding
the objdump filename heading), SHA256
`c97cee55797b778ad59d115f18f9c558810260341db64ac2ba24ed62d8069955`.
Thus the recovered old `ou-m5ops.h` header and compile/link recipe reproduce the
original control's executable code before the diagnostic is added. This does
not, on its own, prove runtime bandwidth correctness.

Minimal repack tests check exact non-target record/metadata preservation and
reject missing or duplicate target entries. Together with reporting-function
tests, five tests passed in **0.69 seconds**:

```sh
python3 -m unittest discover -s tools -p 'test_perftest*py'
```

Use explicit `UBSIM_INITRD` to select the independent diagnostic image, and
`UBSIM_BW_REPORT_DIAGNOSTIC=1` on both endpoints. Normal source, bundle/pin, image,
and ELF remain unchanged. Guest runtime diagnosis is recorded separately.

## SEND runtime result: asymmetric peak policy, not arithmetic corruption

Evidence: `experiments/qemu-send-4k-report-diagnostic-20261009/send-4096.txt`.
The 4096 B, 16384 iteration bidirectional run reports:

| Endpoint | no_peak | Window, counter ticks | Local peak MiB/s | Local average MiB/s |
| --- | --- | --- | --- | --- |
| Server | 1 | 8239638 | 0 | 485.45822037327366 |
| Client | 0 | 2054835 | 1953.125 | 1946.6283180888004 |

Both use `cpu_mhz=62.5`, hence 62500000 counter ticks/second. The merged average
is 2432.086538462074 MiB/s; the merged peak is 1953.125 MiB/s. The client alone
satisfies peak >= average. There is no contradiction: the server contributes
average bandwidth but **no peak measurement**.

The pinned official `perftest_parameters.c:1081` sets `no_peak=true` whenever
the operation is SEND (or immediate-enabled), the test is BW, and
`server_ip == NULL`; it does not exempt bidirectional operation.
`print_bi_bw_report` sums each local field without marking the asymmetric peak
policy. Our earlier mathematical concern assumed both endpoints measured peak;
the runtime evidence disproves that premise. It does not indicate an obsolete
reporting binary, different MiB units, or integer division failure.

The approximately fourfold average-window difference is also a reporting-window
difference, not fourfold clock-rate disagreement. In `run_once_bi_bw`, no-peak
mode timestamps immediately before the main loop and after the whole loop.
The loop drains both transmit completions and receive traffic. Peak-enabled
mode instead uses the first send-post timestamp through the final send-CQ
timestamp. The client may then continue receiving/draining work without extending
its measured send interval. The measured windows are 131.834208 ms and
32.87736 ms, respectively. Runtime diagnostics establish these boundaries and
equal scales; they do not identify which specific receive or drain stall accounts
for the remaining interval without additional event evidence.

Acceptance guidance: accept completed SEND traffic and the explicitly identified
official aggregate-average metric, subject to normal device/network error and
completion checks. Do not treat this merged peak as a bidirectional peak, apply
`aggregate_peak >= aggregate_average` as an invariant, or call the summed local
averages simultaneous goodput over one common wall-clock interval. Physical
throughput/saturation claims still require a shared ROI and byte/time counters.
No official formulas or clock sources were changed to make these numbers agree.

For a later coordinated Linux build, the existing perftest target's
`CMakeFiles/urma_perftest.dir/flags.make`, `build.make`, and `link.txt` preserve
the compiler/linker invocation. A valid incremental build can recompile only
`perftest_run_test.c.o`, then relink `urma_perftest`; the other four application
objects and official libraries are unchanged. The cached source directory
names an old `sources/OpenURMA/...` location, so it must be mapped to the actual
pinned UMDK tree rather than blindly invoking the stale build cache. The normal
`build_umdk.sh` entry point deletes its build directory and is not incremental.

Package into a separate output image with `ARM_BUILD` selecting the diagnostic
build and `OUT` selecting a new filename, then verify its manifest/ELF hashes.
Launch only after coordination, using explicit `UBSIM_INITRD` to select that
image. Enable the variable for both perftest endpoints. Keep the normal image
and evidence intact; do not claim the regular guest includes this overlay.
