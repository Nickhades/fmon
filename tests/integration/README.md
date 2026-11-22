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

- `.github/workflows/ci.yml` — FreeBSD build, tests and static checks.
- `.github/workflows/asan-soak.yml` — manual ASAN unit tests, GUI soak and H-01.

The workflows use the external `vmactions/freebsd-vm` action and have not
been executed from this session.
