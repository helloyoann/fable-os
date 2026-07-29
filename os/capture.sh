#!/bin/sh
# Boot the kernel headless, capture a framebuffer screenshot and the serial log.
#
#   ./capture.sh [seconds-before-shot]
#
# Outputs (override with OUT_PREFIX):
#   $OUT_PREFIX.png   framebuffer screenshot   (default /tmp/os.png)
#   $OUT_PREFIX.log   serial log               (default /tmp/os.log)
#
# CONCURRENCY: this script is safe to run while other QEMU instances (other
# developers, agents, or the tests/qemu harness) are running. It never kills
# processes it does not own, and every path and the QEMU monitor port are
# derived from the PID so parallel runs cannot collide. Set KILL_OTHERS=1 to
# opt into the old behaviour of pkill-ing every qemu-system-x86_64 first.
set -e

DELAY="${1:-3}"
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_PREFIX="${OUT_PREFIX:-/tmp/os}"

# Unique per run: a stale or concurrent instance can't steal our port or files.
# No ".ppm" suffix: appending one would name a file mktemp did not create, and
# the cleanup trap below would then remove the name while leaking the inode.
# QEMU's screendump does not care about the extension.
PPM="$(mktemp -t talkos-shot)"
QLOG="$(mktemp -t talkos-qemu)"
SERIAL="${OUT_PREFIX}.log"
PNG="${OUT_PREFIX}.png"
# Monitor port derived from PID, kept inside the ephemeral range.
PORT=$(( 49152 + ($$ % 16000) ))

if [ "${KILL_OTHERS:-0}" = "1" ]; then
    pkill -f qemu-system-x86_64 2>/dev/null || true
fi

rm -f "$PPM" "$PNG" "$SERIAL"

QPID=""
cleanup() {
    # Only ever kill the QEMU we started ourselves.
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    rm -f "$PPM" "$QLOG" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

qemu-system-x86_64 -kernel "$DIR/kernel.bin" \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SERIAL" \
    -display none -monitor "tcp:127.0.0.1:$PORT,server,nowait" >"$QLOG" 2>&1 &
QPID=$!

PPM="$PPM" PORT="$PORT" python3 - "$DELAY" <<'EOF'
import os, socket, sys, time

delay = float(sys.argv[1])
ppm   = os.environ["PPM"]
port  = int(os.environ["PORT"])

time.sleep(delay)
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
except OSError as e:
    print(f"capture: could not reach QEMU monitor on {port}: {e}", file=sys.stderr)
    sys.exit(0)          # serial log is still useful; don't fail the run
with s:
    s.settimeout(10)
    time.sleep(0.3)
    try: s.recv(4096)
    except OSError: pass
    s.sendall(f"screendump {ppm}\n".encode())
    time.sleep(1.0)
    try: s.recv(4096)
    except OSError: pass
    s.sendall(b"quit\n")
    time.sleep(0.3)
EOF

wait "$QPID" 2>/dev/null || true
QPID=""

sips -s format png "$PPM" --out "$PNG" >/dev/null 2>&1 || true
echo "screenshot: $PNG"
echo "serial log: $SERIAL"
echo "===== serial ====="
cat "$SERIAL" 2>/dev/null || true
