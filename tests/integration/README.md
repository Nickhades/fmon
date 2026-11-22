# Integration tests

These tests need a running fmon daemon and, for some cases, root.

## Control channel permission

```sh
sudo ./tests/integration/control_permission.sh
```

- operator user (current root) must get `OK pong`;
- a non-operator user (default `nobody`) must be rejected.

This validates the socket file permission / local peer boundary. For the
daemon-side `getpeereid()` fail-closed path, the unit-level LD_PRELOAD test is
in the review report.

## ASAN GUI soak

```sh
./tests/integration/gui_asan_soak.sh 3600
```

Runs the ASAN-built GUI against the current daemon, switches pages, and fails on
an AddressSanitizer diagnostic. It is not a substitute for a full desktop
automation suite.

## Root-only fault injection

Real `mdconfig`/ZFS fault injection is environment-specific. On FreeBSD 15.1,
`zpool suspend/resume` is not a supported command; use an isolated test pool
and the fake-command tests described in `CODE_REVIEW.md` for reproducible
blocking-I/O coverage.

## H-01 disk hotplug

```sh
sudo DISPLAY=:0 ./tests/integration/h01_disk_hotplug.sh
```

Attaches an isolated md disk, waits for disk history, detaches it, and verifies
the SQLite history has no rows for the removed device after the detach time.

## CI

GitHub CI is on `main`:

- `.github/workflows/ci.yml` — FreeBSD build and tests.
- `.github/workflows/asan-soak.yml` — manual/nightly ASAN unit tests, GUI soak and H-01.

The build/test workflow and the ASAN/H-01 workflow have passed on the GitHub
FreeBSD runner. The GUI dialog smoke is kept out of the ASAN job because modal
window cleanup under a window-manager-less Xvfb is not reliable; run it locally
or in a dedicated WM-backed job.

## GUI dialog smoke

```sh
DISPLAY=:0 ./tests/integration/gui_dialog_smoke.sh
```

Opens/closes About, alert settings and the CSV save dialog; requires XTest
headers (`libXtst`) and a running daemon. It is intentionally a smoke test, not
a full accessibility-driven dialog suite.
