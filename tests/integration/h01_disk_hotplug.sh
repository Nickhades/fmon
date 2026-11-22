#!/bin/sh
# H-01: attach an md disk, let disk_history write a bucket, detach it,
# and verify no new rows are written for the removed device.
# Must run as root (or with sudo -n); a graphical session is required.
set -eu

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
    GUI_USER=""
else
    SUDO="${FMON_SUDO:-sudo}"
    GUI_USER="$(id -un)"
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
GUI="${FMON_GUI_BIN:-$ROOT/build/src/gui/fmon-gui}"
CACHE="${FMON_H01_CACHE:-/tmp/fmon-h01-cache}"
WAIT="${FMON_H01_WAIT:-75}"

[ -x "$GUI" ] || { echo "missing $GUI" >&2; exit 2; }
[ -n "${DISPLAY:-}" ] || { echo "DISPLAY is required" >&2; exit 2; }

cleanup() {
    if [ -n "${md0:-}" ]; then
        $SUDO /sbin/mdconfig -d -u "$md0" >/dev/null 2>&1 || true
    fi
    if [ -n "${pid:-}" ]; then
        kill -TERM "$pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

pkill -x fmon-gui 2>/dev/null || true
sleep 1
rm -rf "$CACHE"
mkdir -p "$CACHE"

md0=$($SUDO /sbin/mdconfig -a -t swap -s 64m 2>/dev/null | tail -1)
[ -n "$md0" ] || { echo "mdconfig failed" >&2; exit 1; }
echo "attached $md0"

if [ -n "$GUI_USER" ]; then
    chown -R "$GUI_USER" "$CACHE" 2>/dev/null || true
    XDG_CACHE_HOME="$CACHE" "$GUI" --page storage >/tmp/fmon-h01-gui.log 2>&1 &
else
    XDG_CACHE_HOME="$CACHE" "$GUI" --page storage >/tmp/fmon-h01-gui.log 2>&1 &
fi
pid=$!
sleep "$WAIT"

date +%s >/tmp/fmon-h01-remove.time
$SUDO /sbin/mdconfig -d -u "$md0"
echo "detached $md0"
sleep "$WAIT"

kill -TERM "$pid" 2>/dev/null || true
sleep 2
kill -KILL "$pid" 2>/dev/null || true
pid=""

DB=$(find "$CACHE" -name disk_history.db -print -quit)
[ -n "$DB" ] || { echo "no disk_history.db" >&2; exit 1; }

rows=$(sqlite3 "$DB" "select ts from disk_minute where dev='$md0' order by ts;")
late=$(sqlite3 "$DB" "select count(*) from disk_minute where dev='$md0' and ts>$(cat /tmp/fmon-h01-remove.time);")
echo "rows: $rows"
if [ "$late" -ne 0 ]; then
    echo "H-01 regression: $late rows after remove" >&2
    exit 1
fi
echo "H-01 passed: no rows after device removal"
