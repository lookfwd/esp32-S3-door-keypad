# door-keypad tools

Helper scripts for building, flashing and testing the firmware in `../door-keypad`.
They need only Python 3 (standard library), `arduino-cli` and the ESP32 Arduino
core 3.3.12. Run them from the project root (`door-keypad/`, the folder that holds
`tools/`). Passwords are read from `door-keypad/secrets.h` and never printed.

```bash
ESPOTA=~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12/tools/espota.py
BOARD=192.0.2.10    # your keypad board's IP (door-keypad.local resolves only during the update window)
arduino-cli compile --output-dir build door-keypad    # board options come from sketch.yaml
```

| Script | What it does |
|---|---|
| `flash_any.py BOARD SECRETS_H BUILD_DIR SKETCH_DIR ESPOTA` | Waits up to 10 min, then installs `BUILD_DIR`: over USB if the board sits in download mode for 2 s, or over OTA (4 tries) when a normal boot opens the 60 s window. |
| `tlog.py BOARD [WAIT_S] [READ_S]` | Connects to the telnet log as soon as the window opens and prints it (recent lines are replayed). Without `READ_S` it disconnects after 45 s with no log output; while connected, OTA stays available. |
| `door_watch.py BOARD [MINUTES]` | Follows the telnet log across reboots for `MINUTES` (default 20): reconnects whenever a new update window opens and notices a reboot within about 10 s. Holding the connection keeps OTA available. |
| `window_test.py BOARD SECRETS_H BIN ESPOTA [--skip-phase0]` | Automated checks of the OTA/telnet window: 11, or 13 with phase 0. Reboots the board via OTA once, or twice with phase 0. |
| `keypad_test.py BOARD` | Interactive: holds the window open and prints the board log while you type the code, mash keys until the rate limiter drops them, and type the code again once the bucket has drained. Checks from the log that `openDoor()` ran when it should and not while keys were being dropped (9 checks). |
| `limiter_test.cpp` | Host unit test of `KeyRateLimiter.h`. |
| `mon.py PORT SECONDS [--reset]` | Serial monitor for a UART port (DevKitC "UART" port, or a USB-UART adapter on GPIO43/44). `--reset` pulses EN via RTS. |

Typical update after a code change:

```bash
arduino-cli compile --output-dir build door-keypad
python3 -I tools/flash_any.py $BOARD door-keypad/secrets.h build door-keypad $ESPOTA
# now press RESET on the board (or power it on); it installs within the 60 s window
```

Limiter unit test:

```bash
clang++ -std=c++17 -Wall -Wextra -I door-keypad tools/limiter_test.cpp -o /tmp/limiter_test && /tmp/limiter_test
```

USB download mode on the ESP32-S3 Super Mini: hold BOOT, tap RESET, keep BOOT
held about 1 s. After flashing, tap RESET again: the reset at the end of the
upload does not leave download mode that was entered with the button.
