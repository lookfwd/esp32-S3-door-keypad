#!/usr/bin/env python3
"""flash_any.py IP SECRETS_H BUILD_DIR SKETCH_DIR ESPOTA [WAIT_MIN]
Wait up to WAIT_MIN minutes (default 10) for either:
  - the ROM download-mode USB port, present continuously for 2 s  -> flash over USB
  - the OTA/telnet window (telnet port 23 reachable)            -> OTA with retries
Exit 0 once one of them flashed BUILD_DIR successfully."""
import glob, re, socket, subprocess, sys, time

IP, SECRETS, BUILD, SKETCH, ESPOTA = sys.argv[1:6]
WAIT_S = float(sys.argv[6]) * 60 if len(sys.argv) > 6 else 600
PW = re.search(r'#define\s+OTA_PASSWORD\s+"([^"]*)"', open(SECRETS).read()).group(1)
BIN = f"{BUILD}/door-keypad.ino.bin"


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def telnet_open():
    try:
        socket.create_connection((IP, 23), timeout=0.8).close()
        return True
    except OSError:
        return False


def usb_flash(port):
    log(f"USB: flashing via {port}")
    p = subprocess.run(["arduino-cli", "upload", "-p", port, "--input-dir", BUILD, SKETCH], capture_output=True, text=True)
    out = p.stdout + p.stderr
    for l in out.splitlines():
        if re.search(r"Connected to|Hash of data|Hard reset|rror", l) and "operation not permitted" not in l:
            print("   " + l, flush=True)
    return p.returncode == 0


def ota_try():
    t0 = time.time()
    p = subprocess.run([sys.executable, "-I", ESPOTA, "-i", IP, "-p", "3232", f"--auth={PW}", "-f", BIN, "-t", "3"],
                       capture_output=True, text=True)
    out = (p.stdout + p.stderr).replace(PW, "<pw>")
    log(f"OTA attempt: exit {p.returncode} after {time.time() - t0:.1f} s")
    if p.returncode:
        print("\n".join("   " + l for l in out.splitlines() if not l.startswith("Uploading"))[-500:], flush=True)
    return p.returncode == 0


log("waiting for download mode (hold BOOT through RESET) or a normal boot window")
deadline = time.time() + WAIT_S
port_since, last_port = None, None
while time.time() < deadline:
    ports = glob.glob("/dev/cu.usbmodem*")
    if ports:
        if ports[0] != last_port:
            port_since, last_port = time.time(), ports[0]
            log(f"USB port appeared: {last_port}")
        elif time.time() - port_since >= 2.0:
            if usb_flash(last_port):
                log("USB flash OK")
                sys.exit(0)
            log("USB flash failed; waiting again")
            port_since, last_port = None, None
    else:
        if last_port:
            log(f"USB port vanished after {time.time() - port_since:.1f} s (normal boot, not download mode)")
        port_since, last_port = None, None
        if telnet_open():
            log("OTA window is open, trying OTA")
            for _ in range(4):
                if ota_try():
                    log("OTA OK")
                    sys.exit(0)
                if not telnet_open():
                    break
            log("OTA did not get through; waiting for another chance")
            while telnet_open() and time.time() < deadline:
                time.sleep(1)
    time.sleep(0.3)
log(f"gave up after {WAIT_S / 60:.0f} min")
sys.exit(1)
