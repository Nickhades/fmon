#!/bin/sh
# GUI dialog smoke test: About / Alert settings / Export dialog.
# Requires a running daemon and an X display.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
GUI="${FMON_GUI_BIN:-$ROOT/build/src/gui/fmon-gui}"
CLICK="${FMON_XCLICK:-/tmp/fmon-test-xclick}"
CLOSE="${FMON_XCLOSE:-/tmp/fmon-test-xclose}"
CFG=/tmp/fmon-gui-dialog-config
CACHE=/tmp/fmon-gui-dialog-cache

[ -x "$GUI" ] || { echo "missing GUI: $GUI" >&2; exit 2; }
[ -n "${DISPLAY:-}" ] || { echo "DISPLAY required" >&2; exit 2; }

if [ ! -x "$CLICK" ]; then
    cc -O2 -I/usr/local/include -L/usr/local/lib -o "$CLICK" \
        "$ROOT/tests/integration/x11_click.c" -lXtst -lX11
fi
if [ ! -x "$CLOSE" ]; then
    cc -O2 -I/usr/local/include -L/usr/local/lib -o "$CLOSE" \
        "$ROOT/tests/integration/x11_close.c" -lX11
fi

rm -rf "$CFG" "$CACHE"
mkdir -p "$CFG/fmon" "$CACHE"
printf '[window]\nwidth=1500\nheight=920\n' >"$CFG/fmon/window.ini"

pkill -x fmon-gui 2>/dev/null || true
sleep 1
XDG_CONFIG_HOME="$CFG" XDG_CACHE_HOME="$CACHE" "$GUI" --page cpu --theme light \
    >/tmp/fmon-gui-dialog.log 2>&1 &
pid=$!
trap 'kill -TERM "$pid" 2>/dev/null || true' EXIT INT TERM
sleep 4

main=$(xwininfo -root -tree | grep 'FreeBSD 监控中心' | tail -1 | awk '{print $1}')
[ -n "$main" ] || { echo "main window not found" >&2; exit 1; }
left=$(xwininfo -id "$main" | awk '/Absolute upper-left X/{print $4}')
top=$(xwininfo -id "$main" | awk '/Absolute upper-left Y/{print $4}')
w=$(xwininfo -id "$main" | awk '/Width/{print $2}')
h=$(xwininfo -id "$main" | awk '/Height/{print $2}')

click_rel() {
    x=$((left + w * $1 / 1000))
    y=$((top + h * $2 / 1000))
    "$CLICK" "$x" "$y"
}

find_title() {
    xwininfo -root -tree | grep -E "$1" | tail -1 | awk '{print $1}'
}

check_dialog() {
    needle="$1"
    shift
    click_rel "$@"
    sleep 1
    id=$(find_title "$needle" || true)
    if [ -z "$id" ]; then
        echo "dialog not found: $needle" >&2
        exit 1
    fi
    echo "opened: $needle"
    "$CLOSE" "$id"
    sleep 1
}

# Approximate toolbar positions for the fixed 1500x920 test window.
check_dialog '关于 FreeBSD 监控中心|About FreeBSD Monitoring Center' 850 135
check_dialog '告警设置|Alert Settings' 880 135
check_dialog '保存监控数据 CSV|Save monitoring data CSV' 945 90

"$CLOSE" "$main" 2>/dev/null || true
sleep 2
kill -TERM "$pid" 2>/dev/null || true
echo "GUI dialog smoke passed"
