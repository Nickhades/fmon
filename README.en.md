# fmon — FreeBSD System Monitor

A native FreeBSD system monitor and resource manager written in C11 with GTK4.

## What's new in 1.3.2

- **Bilingual UI**: switch between 中文 and English at runtime; page/theme preserved.
- **Full NIC models**: 192-byte model names, shown as a full-width row; ABI 24 requires daemon/GUI upgrade together.
- **Live disk history**: only currently attached disks appear; removed external disks disappear from the history selector.
- **Window behavior**: remembers normal size, centers on X11, and closes without a 1-second collector wait.
- **Truthful data**: missing zpool/metrics show `—`, not fake 0; NaN gaps remain gaps.
- **UI cleanup**: top page tabs, direct quick actions, deeper dark background, fixed light-theme checkbox.
- **Reliability**: process crash/right-click fixes, modular C11 code, 20/20 tests.

Full details: [1.3.2 release notes](docs/RELEASE_NOTES_1.3.2.md).

## Architecture

```
fmon-daemon (root)  --POSIX shared memory (seqlock)-->  fmon-gui (user)
  CPU / memory / disk / network / ZFS / processes          GTK4 + Cairo
  /var/run/fmon-ctl.sock  <---- privileged actions --------
```

- **Daemon**: samples at 1 Hz and publishes immutable snapshots through shared memory.
- **GUI**: maps shared memory read-only; a crash of the GUI never affects collection.
- **Control channel**: privileged operations (signals, services, sysrc, cpuset, rctl) are validated and executed by the root daemon over a local Unix socket. No shell interpolation.

![fmon architecture](docs/project-structure.png)

## Screenshots

![fmon CPU](docs/screenshots/01-cpu.png)
![fmon processes](docs/screenshots/02-process.png)
![fmon ZFS](docs/screenshots/03-zfs.png)
![fmon history](docs/screenshots/04-history.png)

## Features

- CPU: total/per-core utilization, load average, frequency, temperature, thermal history.
- Memory: physical memory, swap, ARC, page/VM counters.
- Storage: disks, GEOM type/interface, USB link speed, mount points, inode/RO status.
- ZFS: pools, ARC size/hit rate/L2ARC/prune/ghost cache, per-pool I/O.
- Network: interfaces, addresses, TCP/UDP connection table with process/PID mapping.
- Processes: sortable list, tree view, multi-select batch actions, signals, nice, cpuset, open files, sockets, parent jump, CSV export.
- Services: rc.d start/stop/restart/reload (when supported), enable/disable via sysrc, status, logs, dependency tree, PID jump, batch operations.
- Logs: multiple sources (messages, auth, security, all, cron, debug, dmesg, zpool events, pkg and jail logs), level/time filters, regex, dedup, PID links, context expansion, export.
- Battery: health, design/full capacity, cycles, voltage/current, dual-axis history.
- Alerts: thresholds, desktop notifications, history, silence.

## Install from the package

```sh
sudo pkg install ./fmon-1.3.2.pkg
sudo sysrc fmon_daemon_enable=YES
sudo service fmon_daemon start
fmon-gui
```

Uninstall:

```sh
sudo service fmon_daemon stop
sudo sysrc fmon_daemon_enable=NO
sudo pkg delete fmon
```

## Build from source

```sh
meson setup build
ninja -C build
meson test -C build
sudo ./install.sh
```

Requirements: FreeBSD 15+, C11 compiler, Meson/Ninja, GTK4, Cairo, SQLite3, libdrm (optional AMD GPU sensors).
The installer auto-detects and installs `dmidecode`, `smartmontools`, and `libdrm` when missing; `--skip-deps` disables this.

## Repository layout

- `src/daemon` — collector and root control daemon.
- `src/gui` — GTK4 interface, charts, views.
- `src/common` — shared-memory ABI and helpers.
- `tests` — unit tests (14 test suites).
- `dist/` — prebuilt FreeBSD package and source archive.
- `docs/` — additional documentation.
- [`docs/CHARTS.md`](docs/CHARTS.md) — why the charts are linear, gap-aware and peak-preserving.

## License

BSD-2-Clause. See [LICENSE](LICENSE).

---
