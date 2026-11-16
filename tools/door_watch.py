#!/usr/bin/env python3
"""door_watch.py BOARD_IP [MINUTES]
Follows the board's telnet log across reboots for MINUTES (default 20):
connects whenever the update window is open, prints every line with a
timestamp, and notices within ~10 s when the board has rebooted (TCP
keepalive), then waits for the next boot's window and reconnects. Each new
boot's replayed log starts with its reset reason."""
import socket, sys, time

BOARD = sys.argv[1]
minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 20
deadline = time.time() + minutes * 60


def log(kind, text):
    print(f"[{time.strftime('%H:%M:%S')}] {kind:5} {text}", flush=True)


def connect():
    s = socket.create_connection((BOARD, 23), timeout=1.5)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    # macOS names the idle-time option TCP_KEEPALIVE; Linux calls it TCP_KEEPIDLE.
    idle = getattr(socket, "TCP_KEEPIDLE", getattr(socket, "TCP_KEEPALIVE", None))
    if idle is not None:
        s.setsockopt(socket.IPPROTO_TCP, idle, 4)
    for name, val in (("TCP_KEEPINTVL", 2), ("TCP_KEEPCNT", 3)):
        if hasattr(socket, name):
            s.setsockopt(socket.IPPROTO_TCP, getattr(socket, name), val)
    s.settimeout(0.5)
    return s


log("WATCH", f"following {BOARD} for {minutes:.0f} min; waiting for an update window")
sessions = 0
while time.time() < deadline:
    try:
        s = connect()
    except OSError:
        time.sleep(0.3)
        continue
    sessions += 1
    log("WATCH", f"session {sessions}: connected")
    buf = ""
    while time.time() < deadline:
        try:
            b = s.recv(4096)
        except socket.timeout:
            continue
        except OSError as e:
            log("WATCH", f"session {sessions}: connection lost ({e}); the board probably rebooted")
            break
        if not b:
            log("WATCH", f"session {sessions}: board closed the connection")
            break
        buf += b.decode("utf-8", "replace")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            if line.strip():
                log("BOARD", line.rstrip())
    s.close()
    if time.time() < deadline:
        log("WATCH", "waiting for the next update window")
log("WATCH", f"done after {sessions} session(s)")
