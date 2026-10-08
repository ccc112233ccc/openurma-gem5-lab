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

## Pending ARM build and packaging

The current regular image still contains the unchanged ELF with SHA256
`7724a94b9e10970bf18a40f45be11a36e19d316fec4a9b0d0f7a0444de9fa84e`.
No Docker daemon, ARM build, image replacement, or diagnostic guest experiment
was started in this task.

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
