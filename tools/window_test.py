#!/usr/bin/env python3
"""End-to-end test of the OTA/telnet window.
Usage: window_test.py BOARD_IP SECRETS_H NEW_BIN ESPOTA [--skip-phase0]
Phase 0 (unless skipped) waits for a boot window and installs NEW_BIN over OTA.
Phases 1-2 check the 60 s window, its extension while telnet is connected,
OTA during the extension, disconnect handling, and that OTA closes afterwards.
Each OTA reboots the board."""
import re, socket, subprocess, sys, time

IP, SECRETS, NEW_BIN, ESPOTA = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
PW = re.search(r'#define\s+OTA_PASSWORD\s+"([^"]*)"', open(SECRETS).read()).group(1)
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(f"{'PASS' if ok else 'FAIL'}: {name} {detail}", flush=True)


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def connect(wait_s):
    t0 = time.time()
    while time.time() - t0 < wait_s:
        try:
            return socket.create_connection((IP, 23), timeout=1.5)
        except OSError:
            time.sleep(0.3)
    return None


def read_for(s, secs, until=None):
    """Read for up to secs; stop early once `until` regex appears. Returns (text, closed_by_peer)."""
    s.settimeout(0.5)
    buf, t0 = "", time.time()
    while time.time() - t0 < secs:
        try:
            b = s.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            return buf, True
        if not b:
            return buf, True
        chunk = b.decode("utf-8", "replace")
        sys.stdout.write(chunk)
        sys.stdout.flush()
        buf += chunk
        if until and re.search(until, buf):
            return buf, False
    return buf, False


def boot_time_from(text):
    m = re.search(r"\((\d+) s left\)", text)
    return time.time() - (60 - int(m.group(1))) if m else None


def ota(bin_path, attempts=1):
    for i in range(1, attempts + 1):
        t0 = time.time()
        p = subprocess.run([sys.executable, "-I", ESPOTA, "-i", IP, "-p", "3232", f"--auth={PW}", "-f", bin_path, "-t", "3"],
                           capture_output=True, text=True)
        out = (p.stdout + p.stderr).replace(PW, "<pw>")
        tail = "\n".join(l for l in out.splitlines() if not l.startswith("Uploading"))[-400:]
        log(f"espota attempt {i}: exit {p.returncode} after {time.time() - t0:.1f} s")
        if p.returncode == 0:
            return 0, tail
        print(tail, flush=True)
    return p.returncode, tail


def port_refused():
    try:
        socket.create_connection((IP, 23), timeout=3).close()
        return False
    except OSError:
        return True


# ---- phase 0: wait for the user's RESET, confirm previous OTA, push the new build
if "--skip-phase0" not in sys.argv:
    log("phase 0: waiting up to 10 min for a boot window (press RESET)")
    s = connect(600)
    if not s:
        check("boot window seen", False)
        sys.exit(1)
    text, _ = read_for(s, 2.0)
    s.close()
    check("previous OTA build is running (log replay present)", "[boot] door-keypad" in text)
    log("phase 0: OTA upload of the 60 s + extension build")
    rc, tail = ota(NEW_BIN, attempts=4)
    check("OTA of new build", rc == 0, f"(espota exit {rc})")
    if rc != 0:
        print(tail)
        sys.exit(1)

# ---- phase 1: stay connected past 60 s, refuse 2nd client, OTA during extension
time.sleep(3)
log("phase 1: connect after OTA reboot")
a = connect(40)
check("telnet after OTA reboot", a is not None)
if not a:
    sys.exit(1)
text, _ = read_for(a, 3.0)
check("new build running (60 s window greeting)", "whichever is later" in text)
boot = boot_time_from(text)
log(f"phase 1: board booted ~{time.time() - boot:.0f} s ago; holding the connection past 60 s")
text2, closed = read_for(a, max(5, boot + 70 - time.time()), until=r"stay open until this telnet client disconnects")
check("window extended at 60 s while connected", "stay open until this telnet client disconnects" in text2 and not closed,
      f"(at ~{time.time() - boot:.0f} s after boot)")
time.sleep(2)
check("second telnet client refused during extension", port_refused())
log(f"phase 1: OTA during extension at ~{time.time() - boot:.0f} s after boot")
rc, tail = ota(NEW_BIN, attempts=4)
check("OTA works during extension", rc == 0, f"(espota exit {rc})")
if rc != 0:
    print(tail)
a.close()

# ---- phase 2: connect + disconnect early, reconnect, disconnect during extension -> closed
time.sleep(3)
log("phase 2: connect after reboot, then disconnect at once")
b = connect(40)
check("telnet after 2nd OTA reboot", b is not None)
if not b:
    sys.exit(1)
text, _ = read_for(b, 2.0)
boot = boot_time_from(text)
b.close()
time.sleep(3)
log("phase 2: reconnect inside the base window")
c = connect(10)
check("reconnect inside base window", c is not None)
text, _ = read_for(c, 2.0)
check("board noticed the first client's clean disconnect", "[net] telnet client disconnected" in text)
text2, closed = read_for(c, max(5, boot + 70 - time.time()), until=r"stay open until this telnet client disconnects")
check("window extended for the reconnected client", "stay open until this telnet client disconnects" in text2)
time.sleep(3)
log(f"phase 2: disconnecting at ~{time.time() - boot:.0f} s after boot")
c.close()
time.sleep(3)
check("telnet closed after the client left during extension", port_refused())
rc, tail = ota(NEW_BIN)
check("OTA refused after the window closed", rc != 0 and "No response" in tail, f"(espota exit {rc})")

print("\n==== SUMMARY: %d/%d passed" % (sum(ok for _, ok in results), len(results)))
for name, ok in results:
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
