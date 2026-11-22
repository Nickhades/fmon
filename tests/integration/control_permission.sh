#!/bin/sh
# Control-channel permission smoke test. Must run as root.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root" >&2
    exit 77
fi

socket="${1:-/var/run/fmon-ctl.sock}"
user="${2:-nobody}"
msg='FMONCTL 1 PING'

out=$(printf '%s\n' "$msg" | nc -U "$socket")
echo "operator: $out"
case "$out" in
    *OK*pong*) ;;
    *) echo "operator PING failed" >&2; exit 1 ;;
esac

if ! id "$user" >/dev/null 2>&1; then
    echo "user $user not found; skip negative test" >&2
    exit 0
fi

if printf '%s\n' "$msg" | sudo -n -u "$user" nc -U "$socket" >/tmp/fmon-ctl-nobody.out 2>&1; then
    echo "ERROR: $user unexpectedly connected" >&2
    cat /tmp/fmon-ctl-nobody.out >&2
    exit 1
fi

echo "non-operator $user rejected: $(cat /tmp/fmon-ctl-nobody.out)"
