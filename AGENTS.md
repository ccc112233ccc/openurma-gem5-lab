# Repository operating rules

## Full-system validation

- Never cold-boot gem5 for routine feature development, debugging, or
  regression. Use `./lab start-ready ...`; it restores the newest checkpoint
  compatible with the complete requested machine/network manifest.
- Use plain `./lab start ...` only when the task explicitly concerns kernel
  boot, guest initialization, checkpoint creation, or proves that no compatible
  checkpoint can represent the change under test.
- If `start-ready` reports that no compatible checkpoint exists, explain which
  configuration changed and create one coordinated checkpoint once. Do not
  repeatedly pay the cold-boot cost for individual test attempts.
- Keep functional tests on `--no-sync` unless timing validity is the property
  being tested. Use conservative synchronization only for bounded timing smoke
  tests.
- Record wall-clock duration for simulator launches and regressions so changes
  in validation cost remain visible.
