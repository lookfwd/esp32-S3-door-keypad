# ESP32-S3 Door Keypad

A USB Numeric Keypad that opens a door on your existing WiFi network setup. An
ESP32-S3 acts as **USB host** for an ordinary USB HID keypad (can be wireless);
when the right code is typed it calls **`openDoor()`**, a function you fill in
as you see fit for your door, for example an HTTP request to a WiFi relay
that drives a door strike.

```
USB keypad ──USB──▶ ESP32-S3 (USB host) ──openDoor()──▶ your door opener (e.g. a WiFi relay)
```

![Keypad Image](img/keypad.jpg)
## How it works

- **Keypad input.** The firmware uses the ESP32 Arduino core's built-in USB host
  keyboard support (`USBHostHIDKeyboard`, core 3.3.x); no extra libraries. Any
  boot-protocol USB keyboard or keypad works. Keypad digits and top-row digits
  both count. NumLock is ignored and its light stays off.
- **Code entry.** The last digits typed are compared with `CODE`; no Enter key
  is needed. Keys other than digits are ignored (they neither clear the entry
  nor count toward the rate limit); 10 s without a digit clears the entry.
- **Opening.** On a match the firmware calls `openDoor()`, which you write in
  `door-keypad/secrets.h` to do whatever opens your door;
  [`secrets.example.h`](door-keypad/secrets.example.h) has an HTTP example.
  If it returns false it is retried every second for up to 15 s.
- **Brute-force protection.** A leaky bucket counts keystrokes and drains one
  every 2 s. Every digit keystroke adds one, even one that is dropped, up to 100. A
  keystroke is dropped silently if, counting it, the bucket would hold more
  than 30, so 30 keys in a burst get through, then one every 2 s. Once the
  bucket is above 30, anyone who keeps typing faster than that gets nothing
  through; after a flood that fills the bucket to 100, the keypad takes about
  2.4 minutes to accept keys again.
  Implemented in [`door-keypad/KeyRateLimiter.h`](door-keypad/KeyRateLimiter.h).

Why do I have the following features? To help Claude do development.

- **Maintenance window.** For 60 s after boot the board offers:
  - OTA firmware updates (ArduinoOTA, password-protected), as `door-keypad.local`;
  - a read-only telnet log on port 23, which replays recent lines on connect.

  If a telnet client connected in those 60 s stays connected, both stay open
  until it disconnects (TCP keepalive notices a client that vanished). After
  that nothing listens until the next boot. The RGB LED shows cyan while the
  window is open. Typed digits are never logged, only how many.
- **Reset reason.** Each boot logs why the chip restarted (power-on, OTA,
  crash, watchdog, brownout), visible in the telnet log.

## Hardware

| Part | Notes |
|---|---|
| [ESP32-S3 Super Mini](https://www.amazon.com/dp/B0GS283V6F) or almost any ESP32-S3 board | Its only USB-C port is the native USB, so it is the keypad's port. Power the board through its **5V pin**; on the tested board that 5V also reaches the USB-C port and powers the keypad (use a USB-C OTG adapter). The red LED is power, the blue LED is the battery charger (blinks with no battery), the WS2812 RGB LED on GPIO48 is the window indicator. |
| USB HID numeric keypad | Must get 5V on VBUS; without it nothing enumerates at all. |
| Door opener | Anything `openDoor()` can trigger, e.g. a WiFi relay with an HTTP API driving a door strike. |
| [2-in-1 USB C OTG Adapter and Charger, Type C OTG Charging Adapter](https://www.amazon.com/dp/B0FT7BG954) | Provide power and USB-A data on a single USB-C slot |

## Setup

1. Copy [`door-keypad/secrets.example.h`](door-keypad/secrets.example.h) to
   `door-keypad/secrets.h` and fill in the WiFi credentials, an OTA password,
   the code, and `openDoor()` for your door. `secrets.h` is git-ignored.
2. Board settings, "ESP32S3 Dev Module" (core 3.3.12):
   **USB Mode: USB-OTG (TinyUSB)**, **USB CDC On Boot: Disabled**.
   `arduino-cli` reads these from [`door-keypad/sketch.yaml`](door-keypad/sketch.yaml);
   in the Arduino IDE set them under Tools.
3. Build: `arduino-cli compile --output-dir build door-keypad`
4. First flash over USB. On the Super Mini, hold BOOT, tap RESET, keep BOOT held
   about 1 s, upload, then tap RESET again.
5. Later updates go over WiFi: start `tools/flash_any.py`, then reset the board;
   it installs during the 60 s window. See [`tools/README.md`](tools/README.md).

## Project layout

```
door-keypad/                  Arduino sketch
  door-keypad.ino             firmware
  KeyRateLimiter.h            leaky-bucket keystroke limiter
  secrets.example.h           template for secrets.h (copy it; secrets.h is git-ignored)
  sketch.yaml                 board options for arduino-cli
tools/                        flashing and test scripts (see tools/README.md)
```

## Testing

- `tools/limiter_test.cpp`: host unit test of the protection logic.
- `tools/window_test.py`: automated checks of the OTA/telnet window (11, or 13 with phase 0)
  (extension, refusing a second client, OTA during the extension, closing).
- `tools/keypad_test.py`: interactive end-to-end test. Prints the board log
  while you type the code, trigger the protection, and type the code again after
  it, and checks that `openDoor()` ran only when it should.

Developed and tested on an ESP32-S3 Super Mini.

## Security notes

- `openDoor()` is only as safe as what it calls. If your door opener accepts
  unauthenticated commands, anyone on the WiFi can open the door directly and
  skip the keypad; protect it with authentication or a separate network.
- OTA is protected by `OTA_PASSWORD` and is only reachable during the
  maintenance window.
- At one key every 2 s, the fastest pace the bucket lets through indefinitely, a
  single sequence of 10ⁿ + n − 1 digits tries every n-digit code: about 5.6 h
  for 4 digits, 2.3 days for 5, 23 days for 6. Software can mitigate this in many
  ways, but my guess - and unfortunately, my experience - is that a screwdriver
  will be the path of least resistance for a rogue actor.

## Enclosure
You can find enclosure sources in [enclosure](enclosure). It's a concept that you can modify for your setup.

![Enclosure](img/enclosure.png)

## Reference

- [ESP32-S3 Super Mini Reference4 and Schematic (Chinese)](https://www.nologo.tech/product/esp32/esp32s3/esp32s3supermini/esp32S3SuperMini.html#%E5%B0%BA%E5%AF%B8%E5%9B%BE)
- [ESP32-S3 Super Mini on ESPBoards (English)](https://www.espboards.dev/esp32/esp32-s3-super-mini/)

## Project Prompt

```
Complete an ESP32-S3 USB-host door-keypad project including firmware, documentation, a Git repo, and a 3D printable enclosure. Ask only for information or physical actions you genuinely need; do everything else independently.

### Hardware and existing context

The project uses an ESP32-S3 Super Mini:

- https://www.amazon.com/dp/B0GS283V6F
- https://www.espboards.dev/esp32/esp32-s3-super-mini/

The ESP32-S3 acts as a USB host for a numeric keypad. Identify the currently connected board and correct upload interface before programming it. Provide logs and subsequent updates over Wi-Fi.

The keypad needs a proper 5 V supply. This might require cutting unwanted data connections from the power-adapter branch, which appeared to interfere with D+/D−. Do not treat keypad illumination as proof that USB enumeration works.

### Firmware

1. Use the USB keypad to recognize the configured N-digit access code and call `openDoor()` when it matches. Read the current code from the existing private configuration; do not invent a replacement or expose it in output. Flash the latest saved password/configuration.
2. Keep Wi-Fi credentials, the access code, the door-controller address, and the real door-opening implementation in ignored private files.
3. Expose a generic `openDoor()` interface in public code. Keep the real controller-specific implementation private.
4. Implement a leaky-bucket rate limiter with 30 initial keystrokes, draining one counted keystroke every three seconds. When the rate goes beyond 30 keystrokes/minute, ignore keystrokes.
5. Enable OTA updates and a network terminal for one minute after boot. If a terminal session is still open, keep the maintenance window open until that session closes. Once the initial minute has elapsed and no terminal remains connected, shut down both services. Avoid interrupting an active firmware update. A reboot should reopen the window.
6. Indicate the open maintenance window using the onboard NeoPixel on GPIO48, in cyan at 10% brightness. Turn the indicator off when the window closes. Verify the actual color on the connected board: apparently identical boards previously needed different RGB/GRB ordering.
7. Configure Wi-Fi to choose the strongest available access point advertising the configured SSID, using the appropriate ESP-IDF scan/sort settings. Support reconnection appropriately for a mesh network and verify the selected access point and signal strength without exposing identifying network details in committed files.

### Build, flash, and verify

Build and flash the connected board with the final firmware and current private configuration. Verify as much as possible through actual logs and hardware tests:

- USB keypad enumeration, input, and unplug/replug recovery.
- Correct N-digit recognition and rejection of incorrect input.
- Rate limiting, and recovery.
- Wi-Fi access-point selection and reconnection.
- Maintenance-window expiry and terminal-session extension.
- Cyan LED color and brightness.
- Door-opening behavior through the existing private implementation.

Request physical keypad presses, resets, or reconnections only when necessary. Do not report hardware behavior as verified unless it was observed.

### Documentation and Git

Create or update the root `README.md` to explain setup, wiring and power, build/upload settings, private configuration, N-digit operation, rate limiting, LED behavior, and troubleshooting. Give actionable security guidance for the generic door-opening integration.

Initialize Git if needed. Ignore `secrets*.h` except the example configuration header, all other private implementation/configuration files, and generated artifacts including `*.bin`, `*.elf`, and `*.map`. Supply a usable `secrets.example.h` with placeholders and a safe generic `openDoor()` example.

Review the staged files for sensitive network or access information. Check the actual staged diff and tracked files, not just ignore rules. Exclude credentials, access codes, private addresses, identifying logs, and controller-specific details. Commit the reviewed project changes with a descriptive message.

### Enclosure

Create or revise the enclosure using:

- Scan, photos, comments, and measurements provided by the user.
- Resources available for YAPP enclosure designer framework.

Inspect all available references before choosing dimensions. Account for the USB-C pass-through board and its mounting holes, the ESP32 board, wiring, connector access, and assembly clearance. The USB-C plug that connects to the power brick must protrude approximately 2–3 mm beyond the enclosure. Ensure that:

- Base alignment pins printed too large to fit the lid sockets.
- Lid sockets were too fragile, particularly near their tops.

Provide realistic printing clearance, suitable lead-ins, and stronger socket supports. Use the measurements as the dimensional reference and explain any remaining assumptions. Produce editable design files and printable base/lid exports, inspect the geometry and fit, and open the final clean enclosure in FreeCAD (if available). Distinguish digital checks from fit that still needs confirmation with a physical print.
```
