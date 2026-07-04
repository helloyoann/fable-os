#!/bin/sh
# Boot the kernel headless, capture a framebuffer screenshot to /tmp/os.png and
# the serial log to /tmp/serial.log. Usage: ./capture.sh [seconds-before-shot]
set -e
DELAY="${1:-3}"
DIR="$(cd "$(dirname "$0")" && pwd)"
pkill -f qemu-system-x86_64 2>/dev/null || true
rm -f /tmp/os.ppm /tmp/os.png /tmp/serial.log
qemu-system-x86_64 -kernel "$DIR/kernel.bin" \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial file:/tmp/serial.log \
    -display none -monitor tcp:127.0.0.1:55899,server,nowait >/tmp/qemu.log 2>&1 &
QPID=$!
python3 - "$DELAY" <<'EOF'
import socket, time, sys
time.sleep(float(sys.argv[1]))
s = socket.create_connection(('127.0.0.1', 55899))
time.sleep(0.3); s.recv(4096)
s.sendall(b'screendump /tmp/os.ppm\n'); time.sleep(1.0); s.recv(4096)
s.sendall(b'quit\n'); time.sleep(0.3); s.close()
EOF
wait $QPID 2>/dev/null || true
sips -s format png /tmp/os.ppm --out /tmp/os.png >/dev/null 2>&1 || true
echo "screenshot: /tmp/os.png"
echo "===== serial.log ====="
cat /tmp/serial.log 2>/dev/null || true
