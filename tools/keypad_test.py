#!/usr/bin/env python3
"""keypad_test.py BOARD
Interactive end-to-end test. Waits for the board's telnet window, holds it
open, and prints the board log while you type. Finishes once it has seen:
code accepted + "[door] opened", the rate limiter dropping keys, the bucket
draining, and code accepted + "[door] opened" again after it (or after 15 min).

Steps for the person at the keypad:
  1. type the code             -> door opens
  2. mash 40+ keys quickly     -> keys dropped; type the code: nothing happens
  3. wait for "accepted again" in the log, then ~10 s more (room for the
     whole code), type the code -> door opens again"""
import re, socket, sys, time

BOARD = sys.argv[1]
t_start = time.time()
events = []  # (time, kind, text)


def out(kind, text):
    events.append((time.time(), kind, text))
    print(f"[{time.strftime('%H:%M:%S')}] {kind:5} {text}", flush=True)


def seen(pattern, after=0.0, before=None):
    for t, k, txt in events:
        if k == "BOARD" and t >= after and (before is None or t < before) and re.search(pattern, txt):
            return t
    return None


out("TEST", "waiting for the board's OTA/telnet window (power it on with the keypad attached)")
s = None
while time.time() - t_start < 600 and s is None:
    try:
        s = socket.create_connection((BOARD, 23), timeout=1.5)
    except OSError:
        time.sleep(0.3)
if s is None:
    out("TEST", "board window never opened")
    sys.exit(1)
out("TEST", "connected; window held open while this test runs")
s.settimeout(0.5)
buf = ""
while time.time() - t_start < 900:
    try:
        b = s.recv(4096)
        if not b:
            out("TEST", "board closed the connection")
            break
        buf += b.decode("utf-8", "replace")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            if line.strip():
                out("BOARD", line.rstrip())
    except socket.timeout:
        pass
    except OSError as e:
        out("TEST", f"connection error: {e}")
        break
    a1 = seen(r"code accepted")
    o1 = a1 and seen(r"\[door\] opened", a1)
    l1 = seen(r"dropping keystrokes")
    l2 = l1 and seen(r"accepted again", l1)
    a2 = l2 and seen(r"code accepted", l2)
    o2 = a2 and seen(r"\[door\] opened", a2)
    if o1 and l1 and l2 and o2:
        break
s.close()

print("\n==== CHECKS")
results = []
def chk(name, ok):
    results.append(bool(ok))
    print(f"  {'PASS' if ok else '----'}  {name}")
a1 = seen(r"code accepted")
l1 = seen(r"dropping keystrokes")
l2 = l1 and seen(r"accepted again", l1)
a2 = l2 and seen(r"code accepted", l2)
chk("keypad attached", seen(r"keypad attached") is not None)
chk("digits registered", seen(r"\[key\] digit") is not None)
chk("code accepted", a1 is not None)
chk("openDoor() succeeded after the code", bool(a1 and seen(r"\[door\] opened", a1)))
chk("keys dropped above 30", l1 is not None)
chk("no digit accepted while dropping", bool(l1 and l2) and seen(r"\[key\] digit", l1, l2) is None)
chk("door not opened while dropping", bool(l1 and l2) and seen(r"\[door\] opened", l1, l2) is None)
chk("bucket drained and keys accepted again", l2 is not None)
chk("code works again after draining", bool(a2 and seen(r"\[door\] opened", a2)))
if l1 and l2:
    print(f"  (dropping lasted {l2 - l1:.1f} s on this computer's clock: ~2 s per key above 29)")
sys.exit(0 if all(results) else 1)
