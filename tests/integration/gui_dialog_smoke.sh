#!/bin/sh
# GUI dialog smoke test: About / Alert settings / Export dialog.
# Requires a running daemon and an X display.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
GUI="${FMON_GUI_BIN:-$ROOT/build/src/gui/fmon-gui}"
CLICK="${FMON_XCLICK:-/tmp/fmon-test-xclick}"
CLOSE="${FMON_XCLOSE:-/tmp/fmon-test-xclose}"
ESC="${FMON_XESC:-/tmp/fmon-test-xesc}"
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
if [ ! -x "$ESC" ]; then
    cc -O2 -I/usr/local/include -L/usr/local/lib -o "$ESC" \
        "$ROOT/tests/integration/x11_escape.c" -lXtst -lX11
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

main=$(xwininfo -root -tree | grep -E 'FreeBSD 监控中心|FreeBSD Monitoring Center' | grep -v ' 1x1+' | tail -1 | awk '{print $1}')
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
    base_x="$2"
    base_y="$3"
    for dx in 0 -20 20 -40 40 -60 60 -80 80; do
        for dy in 0 -15 15 -30 30; do
            click_rel $((base_x + dx)) $((base_y + dy))
            sleep 0.35
            id=$(find_title "$needle" || true)
            if [ -n "$id" ]; then
                echo "opened: $needle"
                "$ESC" 2>/dev/null || true
                sleep 0.8
                if [ -n "$(find_title "$needle" || true)" ]; then
                    "$CLOSE" "$id"
                    sleep 0.8
                fi
                if [ -n "$(find_title "$needle" || true)" ]; then
                    echo "dialog did not close: $needle" >&2
                    exit 1
                fi
                return 0
            fi
        done
    done
    echo "dialog not found: $needle" >&2
    echo "main geometry: left=$left top=$top w=$w h=$h" >&2
    xwininfo -root -tree >&2 || true
    exit 1
}

# Approximate toolbar positions for the fixed 1500x920 test window.
check_dialog '关于 FreeBSD 监控中心|About FreeBSD Monitoring Center' 863 120
check_dialog '告警设置|Alert Settings' 895 120
check_dialog '保存监控数据 CSV|Save monitoring data CSV' 945 75

"$CLOSE" "$main" 2>/dev/null || true
sleep 2
kill -TERM "$pid" 2>/dev/null || true
echo "GUI dialog smoke passed"
