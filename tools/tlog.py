#!/usr/bin/env python3
"""tlog.py HOST [WAIT_S] [READ_S]: keep trying to connect to HOST:23 for WAIT_S,
then print until the server closes (or for READ_S seconds if given)."""
import socket, sys, time

host = sys.argv[1]
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 30
read_max = float(sys.argv[3]) if len(sys.argv) > 3 else None
t0 = time.time()
while True:
    try:
        s = socket.create_connection((host, 23), timeout=1.5)
        break
    except OSError as e:
        if time.time() - t0 > wait:
            print(f"[tlog] could not connect to {host}:23 within {wait:.0f} s ({e})")
            sys.exit(1)
        time.sleep(0.3)
print(f"[tlog] connected after {time.time() - t0:.1f} s")
s.settimeout(read_max if read_max else 45)
t1 = time.time()
try:
    while True:
        if read_max and time.time() - t1 > read_max:
            print("[tlog] done reading")
            s.close()
            sys.exit(0)
        b = s.recv(4096)
        if not b:
            break
        sys.stdout.write(b.decode("utf-8", "replace"))
        sys.stdout.flush()
except socket.timeout:
    print("[tlog] read timeout")
print(f"[tlog] closed by board after {time.time() - t0:.1f} s")
