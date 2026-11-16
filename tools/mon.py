#!/usr/bin/env python3
"""Minimal serial monitor: mon.py PORT SECONDS [--reset]. Resets via RTS (EN) if --reset."""
import fcntl, os, select, struct, sys, termios, time

port, secs = sys.argv[1], float(sys.argv[2])
reset = "--reset" in sys.argv

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[0] = 0                      # iflag
attrs[1] = 0                      # oflag
attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
attrs[3] = 0                      # lflag
attrs[4] = attrs[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, attrs)

TIOCM_DTR, TIOCM_RTS = 0x002, 0x004
TIOCMBIS, TIOCMBIC = 0x8004746C, 0x8004746B  # macOS
def setbits(bits, on):
    fcntl.ioctl(fd, TIOCMBIS if on else TIOCMBIC, struct.pack("I", bits))

setbits(TIOCM_DTR, False)  # IO0 high (normal boot)
if reset:
    setbits(TIOCM_RTS, True)   # EN low
    time.sleep(0.1)
setbits(TIOCM_RTS, False)      # EN high -> run

end = time.time() + secs
buf = b""
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
os.close(fd)
