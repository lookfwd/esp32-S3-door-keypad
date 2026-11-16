/*
 * door-keypad
 *
 * ESP32-S3 acting as USB host for a USB HID numeric keypad.
 * Typing CODE on the keypad calls openDoor(), which you write in secrets.h to
 * suit your door (secrets.example.h has an HTTP example). secrets.h also holds
 * CODE and the WiFi/OTA passwords, and is git-ignored.
 *
 * Board: "ESP32S3 Dev Module" with
 *   Tools > USB Mode: USB-OTG (TinyUSB)   (Espressif's recommended mode for USB host)
 *   Tools > USB CDC On Boot: Disabled     (must not be Enabled together with USB-OTG)
 * (arduino-cli picks these up from sketch.yaml.)
 *
 * The keypad plugs into the board's native USB port (GPIO19/20) and MUST get 5V
 * on that port's VBUS; without it nothing enumerates at all.
 *
 * ESP32-S3 Super Mini: its only USB-C port is the native one, so it is the
 * keypad's port. Logs and updates go over WiFi, only for NET_WINDOW_MS (60 s)
 * after boot, or for as long after that as the telnet client that connected
 * inside those 60 s stays connected (keepalive detects a vanished client):
 *   - telnet door-keypad.local 23 : read-only copy of the log (replays recent lines)
 *   - ArduinoOTA at door-keypad.local:3232, password OTA_PASSWORD (secrets.h)
 * The RGB LED (GPIO48) is cyan (10%) while the window is open.
 * A successful OTA reboots the board, which opens a new window. To flash over
 * USB instead: unplug the keypad, connect to the computer, hold BOOT, tap RESET,
 * keep BOOT held ~1 s, flash, then tap RESET again (esptool's own reset leaves a
 * manually entered download mode active; `esptool --after watchdog-reset` also works).
 * Serial (115200) also goes to UART0 (TX=GPIO43, RX=GPIO44).
 *
 * ESP32-S3-DevKitC-1: the "UART" port carries Serial; its native "USB" port
 * gives no 5V unless the "USB-OTG" pad is bridged or a powered hub is used.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <esp_system.h>
#include <USBHost.h>
#include <USBHostHIDKeyboard.h>
#include <soc/usb_dwc_struct.h>
#include <tusb.h>  // tuh_hid_set_report(): NumLock light off on attach
#include "KeyRateLimiter.h"

static const char *HOSTNAME = "door-keypad";
static const uint32_t NET_WINDOW_MS = 60000;      // OTA + telnet this long after boot (longer while telnet is connected)
static const uint32_t OTA_RX_TIMEOUT_MS = 5000;   // ArduinoOTA aborts after 3 stalls this long (default 1000)

// Super Mini WS2812 RGB LED on GPIO48: cyan at 10% while the OTA/telnet window is open, off otherwise.
static const uint8_t WINDOW_LED_PIN = 48;
static const uint8_t WINDOW_LED_R = 0, WINDOW_LED_G = 26, WINDOW_LED_B = 26;  // 10% of 255
// Byte order of the board's RGB LED. It varies between Super Mini batches: if cyan
// shows as pink, build with
//   --build-property compiler.cpp.extra_flags=-DWINDOW_LED_ORDER=LED_COLOR_ORDER_GRB
#ifndef WINDOW_LED_ORDER
#define WINDOW_LED_ORDER LED_COLOR_ORDER_RGB
#endif

static const uint32_t ENTRY_TIMEOUT_MS = 10000;   // forget a half-typed code after this idle time
static const uint32_t OPEN_GIVE_UP_MS = 15000;    // stop retrying openDoor() after this
static const uint32_t OPEN_RETRY_MS = 1000;       // pause between openDoor() attempts
static const uint32_t WIFI_KICK_MS = 30000;       // re-begin WiFi if disconnected this long

// Leaky bucket (no lockout): every digit keystroke adds one, up to RATE_CAP_KEYS, and
// one drains every RATE_DRAIN_MS. A keystroke is dropped if, counting it, the
// bucket would hold more than RATE_MAX_KEYS. A flood that fills it to 100
// needs ~2.4 min to drain.
static const uint32_t RATE_MAX_KEYS = 30;
static const uint32_t RATE_CAP_KEYS = 100;
static const uint32_t RATE_DRAIN_MS = 2000;

//-- NumLock light: the core only sends an LED report when its LED state changes,
//-- and that state starts at "off", so on its own it never sends one and the
//-- light stays wherever the keypad left it. This sends "all off" once per
//-- attach. The core runs serviceFromHostTask() only for a bound interface
//-- once the whole device has finished enumerating, and control transfers
//-- must start only from there (sending it straight from loop() raced the
//-- enumeration of a receiver's remaining interfaces).
class KeypadKeyboard : public USBHostHIDKeyboard {
public:
  void requestLedsOff() { _ledsOffPending = true; }
  bool takeLedsOffSent() { bool s = _ledsOffSent; _ledsOffSent = false; return s; }

private:
  volatile bool _ledsOffPending = false, _ledsOffSent = false;
  uint8_t _ledsOffReport = 0;  // has to outlive the transfer
  void serviceFromHostTask() override {  // replaces the core's LED sender (lock handling is off)
    if (!_ledsOffPending) return;
    if (tuh_hid_set_report(devAddr(), interfaceIndex(), 0, HID_REPORT_TYPE_OUTPUT, &_ledsOffReport, 1)) {
      _ledsOffPending = false;
      _ledsOffSent = true;
    }
  }
};
KeypadKeyboard Keypad;
static KeyRateLimiter keyLimiter(RATE_MAX_KEYS, RATE_CAP_KEYS, RATE_DRAIN_MS);
static bool dropEpisode = false;     // logged once when dropping starts and once when it ends
static uint32_t droppedKeys = 0;

static char entry[16];
static size_t entryLen = 0;
static uint32_t lastKeyMs = 0;
static uint8_t prevKeys[6] = {0};

static bool openPending = false;
static uint32_t openRequestedMs = 0;
static uint32_t nextOpenAttemptMs = 0;

static uint32_t wifiDownSinceMs = 0;
static volatile int wifiLastDisconnectReason = 0;  // set from the WiFi event task

// ---------------------------------------------------------------- log

// Everything is logged from loop() context only: to Serial (UART0), to a
// backlog that is replayed to each new telnet client, and, while the network
// window is open, to one telnet client.
class TeeLog : public Print {
public:
  WiFiClient client;

  size_t write(uint8_t c) override {
    return write(&c, 1);
  }
  size_t write(const uint8_t *buf, size_t n) override {
    Serial.write(buf, n);
    for (size_t i = 0; i < n; i++) {
      _ring[_head] = (char)buf[i];
      _head = (_head + 1) % BACKLOG;
      if (_head == 0) _wrapped = true;
    }
    if (client && client.connected()) {
      client.write(buf, n);
    }
    return n;
  }

  /** Send the backlog (oldest first, starting at a line boundary) to the client. */
  void replay() {
    if (!client) return;
    if (_wrapped) {
      size_t start = _head;
      while (start < BACKLOG && _ring[start] != '\n') start++;  // drop the cut-off first line
      if (start + 1 < BACKLOG) client.write((const uint8_t *)_ring + start + 1, BACKLOG - start - 1);
    }
    client.write((const uint8_t *)_ring, _head);
  }

private:
  static const size_t BACKLOG = 2048;
  char _ring[BACKLOG];
  size_t _head = 0;
  bool _wrapped = false;
};
static TeeLog Log;

// ---------------------------------------------------------------- site config

// secrets.h (git-ignored; template: secrets.example.h) holds WIFI_SSID,
// WIFI_PASSWORD, OTA_PASSWORD, CODE and openDoor(). Included here, after Log,
// so that openDoor() can log.
#include "secrets.h"

static const size_t CODE_LEN = strlen(CODE);

// ---------------------------------------------------------------- network window (OTA + telnet)

static WiFiServer telnetServer(23);
// WAITING: no WiFi yet. OPEN: base window, new telnet clients accepted.
// EXTENDED: base window over, kept open only while the telnet client stays connected.
enum NetState { NET_WAITING, NET_OPEN, NET_EXTENDED, NET_CLOSED };
static NetState netState = NET_WAITING;

// Whether a telnet client was accepted and not yet seen to go away. Tracked
// here because WiFiClient's bool/connected() can flip to false on its own,
// which would skip the disconnect handling below.
static bool telnetActive = false;

// Our own liveness check: NetworkClient::connected() treats a clean close
// (recv() == 0) and any errno outside its short list as still connected.
// A keepalive timeout arrives as ECONNABORTED (lwIP ERR_ABRT).
static bool telnetAlive(WiFiClient &c) {
  int fd = c.fd();
  if (fd < 0) return false;
  uint8_t b;
  int r = recv(fd, &b, 1, MSG_DONTWAIT | MSG_PEEK);
  if (r > 0) return true;   // input waiting
  if (r == 0) return false; // client closed the connection
  return errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOENT;  // just no data
}

// Keepalive probes notice a client that vanished without closing (sleep, WiFi
// loss): idle 5 s, then 3 probes 3 s apart -> dead after ~14 s of silence.
static void enableKeepalive(WiFiClient &c) {
  int on = 1, idle = 5, intvl = 3, cnt = 3;
  c.setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  c.setOption(TCP_KEEPIDLE, &idle);
  c.setOption(TCP_KEEPINTVL, &intvl);
  c.setOption(TCP_KEEPCNT, &cnt);
}

static const char *otaErrorName(ota_error_t e) {
  switch (e) {
    case OTA_AUTH_ERROR:    return "auth";
    case OTA_BEGIN_ERROR:   return "begin";
    case OTA_CONNECT_ERROR: return "connect";
    case OTA_RECEIVE_ERROR: return "receive";
    case OTA_END_ERROR:     return "end";
  }
  return "?";
}

static void openNetWindow() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setTimeout(OTA_RX_TIMEOUT_MS);
  ArduinoOTA.onStart([]() { Log.println("[ota] update started"); });
  ArduinoOTA.onEnd([]() { Log.println("[ota] update done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) { Log.printf("[ota] update failed: %s error\n", otaErrorName(e)); });
  ArduinoOTA.begin();
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  netState = NET_OPEN;
  rgbLedWriteOrdered(WINDOW_LED_PIN, WINDOW_LED_ORDER, WINDOW_LED_R, WINDOW_LED_G, WINDOW_LED_B);
  Log.printf("[net] OTA + telnet open at %s.local (%s) until %lu s after boot, longer while telnet is connected\n",
             HOSTNAME, WiFi.localIP().toString().c_str(), (unsigned long)(NET_WINDOW_MS / 1000));
}

static void closeNetWindow(const char *why) {
  Log.printf("[net] closing OTA and telnet (%s)\n", why);
  Log.client.stop();
  telnetActive = false;
  telnetServer.end();
  ArduinoOTA.end();
  netState = NET_CLOSED;
  rgbLedWriteOrdered(WINDOW_LED_PIN, WINDOW_LED_ORDER, 0, 0, 0);
}

static void serviceNetWindow() {
  if (netState == NET_CLOSED) return;
  bool baseOver = millis() >= NET_WINDOW_MS;

  if (netState == NET_WAITING) {
    if (baseOver) {
      netState = NET_CLOSED;  // WiFi never came up inside the window
    } else if (WiFi.status() == WL_CONNECTED) {
      openNetWindow();
    }
    return;
  }

  uint32_t t0 = millis();
  ArduinoOTA.handle();  // blocks during the password check and for the whole upload
  uint32_t took = millis() - t0;
  if (took > 300) {
    Log.printf("[ota] handle() blocked %lu ms\n", (unsigned long)took);
  }

  if (netState == NET_OPEN && !baseOver) {
    WiFiClient c = telnetServer.accept();
    if (c) {
      if (telnetActive) {
        Log.client.stop();
        Log.println("[net] previous telnet client replaced");
      }
      enableKeepalive(c);
      Log.client = c;
      telnetActive = true;
      Log.replay();
      Log.printf("[net] telnet client connected; window ends %lu s after boot (%lu s left) or when you disconnect, "
                 "whichever is later\n",
                 (unsigned long)(NET_WINDOW_MS / 1000), (unsigned long)((NET_WINDOW_MS - millis()) / 1000));
    }
  }

  static uint32_t lastAliveCheckMs = 0;
  if (telnetActive && millis() - lastAliveCheckMs >= 200) {
    lastAliveCheckMs = millis();
    if (!telnetAlive(Log.client)) {
      Log.client.stop();
      telnetActive = false;
      Log.println("[net] telnet client disconnected");
    }
  }
  while (telnetActive && Log.client.available()) {
    Log.client.read();  // read-only console: discard input
  }

  if (!baseOver) return;
  if (netState == NET_OPEN) {
    if (!telnetActive) {
      closeNetWindow("window over");
      return;
    }
    telnetServer.end();  // no new clients; this one keeps the window open
    netState = NET_EXTENDED;
    Log.printf("[net] %lu s window over; OTA + telnet stay open until this telnet client disconnects\n",
               (unsigned long)(NET_WINDOW_MS / 1000));
  } else if (!telnetActive) {
    closeNetWindow("telnet client gone");
  }
}

// ---------------------------------------------------------------- keypad

// HID keyboard-page usage -> digit, for both the keypad block and the top row.
static char usageToDigit(uint8_t u) {
  if (u >= 0x59 && u <= 0x61) return '1' + (u - 0x59);  // Keypad 1..9
  if (u == 0x62) return '0';                            // Keypad 0
  if (u >= 0x1E && u <= 0x26) return '1' + (u - 0x1E);  // 1..9
  if (u == 0x27) return '0';                            // 0
  return 0;
}

static void requestOpen() {
  openPending = true;
  openRequestedMs = millis();
  nextOpenAttemptMs = openRequestedMs;
}

static void serviceRate() {
  keyLimiter.tick(millis());
  if (dropEpisode && !keyLimiter.dropping()) {
    Log.printf("[rate] bucket drained to %.1f/%lu, keystrokes accepted again (%lu dropped)\n",
               keyLimiter.level(), (unsigned long)RATE_MAX_KEYS, (unsigned long)droppedKeys);
    dropEpisode = false;
    droppedKeys = 0;
  }
}

static void onKeyPressed(uint8_t usage) {
  // Only digits count. Every other key is ignored: it neither clears the entry
  // nor fills the rate-limit bucket. (Keypads send extras: NumLock on their
  // own, the FadyDail "00" key sends F13 ahead of its two zeros, Enter, ...)
  char d = usageToDigit(usage);
  if (d == 0) {
    Log.printf("[key] usage 0x%02x ignored (not a digit)\n", usage);
    return;
  }
  serviceRate();
  uint32_t now = millis();
  if (keyLimiter.onKey(now) == KeyRateLimiter::DROP) {
    //-- Dropped silently: the entry is left as it is, so a dropped digit just
    //-- makes the code being typed wrong.
    droppedKeys++;
    if (!dropEpisode) {
      dropEpisode = true;
      Log.printf("[rate] bucket over %lu keys, dropping keystrokes (one drains every %lu s, cap %lu)\n",
                 (unsigned long)RATE_MAX_KEYS, (unsigned long)(RATE_DRAIN_MS / 1000), (unsigned long)RATE_CAP_KEYS);
    }
    return;
  }

  if (entryLen > 0 && now - lastKeyMs > ENTRY_TIMEOUT_MS) {
    entryLen = 0;
  }
  lastKeyMs = now;

  // Keep the last CODE_LEN digits; the code matches anywhere in the stream.
  if (entryLen == CODE_LEN) {
    memmove(entry, entry + 1, CODE_LEN - 1);
    entryLen--;
  }
  entry[entryLen++] = d;
  entry[entryLen] = '\0';

  // Digits are not logged (the telnet log is readable by anyone on the WiFi).
  char masked[sizeof(entry)];
  memset(masked, '*', entryLen);
  masked[entryLen] = '\0';
  Log.printf("[key] digit  entry=%s  bucket=%.1f/%lu\n", masked, keyLimiter.level(), (unsigned long)RATE_MAX_KEYS);

  if (entryLen == CODE_LEN && memcmp(entry, CODE, CODE_LEN) == 0) {
    Log.println("[key] code accepted");
    entryLen = 0;
    requestOpen();
  }
}

// Logs every HID interface the USB host is offered, whether or not the keypad
// handler claims it, so "nothing enumerated" and "enumerated but not a boot
// keyboard" can be told apart. claim() runs in the USB host task: it only
// queues, and logHidInterfaces() prints from loop().
class HidInterfaceLogger : public USBHostHIDDevice {
public:
  bool claim(uint8_t dev_addr, uint8_t idx, uint8_t protocol, const uint8_t *, uint16_t desc_len) override {
    uint8_t w = _w, n = (uint8_t)((w + 1) % SLOTS);
    if (n != _r) {
      _q[w] = {dev_addr, idx, protocol, desc_len};
      _w = n;
    }
    return false;  // never claim; the next handler gets it
  }
  void onUnmount(uint8_t, uint8_t) override {}
  void onReport(uint8_t, uint8_t, const uint8_t *, uint16_t) override {}

  void flush() {
    while (_r != _w) {
      const Entry &e = _q[_r];
      const char *kind = e.protocol == 1 ? "boot keyboard" : e.protocol == 2 ? "boot mouse" : "no boot protocol";
      uint16_t vid = 0, pid = 0;
      tuh_vid_pid_get(e.dev_addr, &vid, &pid);
      Log.printf("[usb] HID interface: device %u (%04x:%04x), interface %u, %s, %u-byte report descriptor\n", e.dev_addr,
                 vid, pid, e.idx, kind, e.desc_len);
      _r = (uint8_t)((_r + 1) % SLOTS);
    }
  }

private:
  struct Entry {
    uint8_t dev_addr, idx, protocol;
    uint16_t desc_len;
  };
  static const uint8_t SLOTS = 8;
  Entry _q[SLOTS];
  volatile uint8_t _w = 0, _r = 0;
};
static HidInterfaceLogger hidLogger;

// Runs from USBHost.task() in loop() context.
static void onKeypadReport(uint8_t modifiers, const uint8_t keys[6], void *) {
  (void)modifiers;
  for (int i = 0; i < 6; i++) {
    uint8_t k = keys[i];
    if (k < 0x04) continue;  // 0 = empty, 1..3 = rollover/error codes
    bool wasDown = false;
    for (int j = 0; j < 6; j++) {
      if (prevKeys[j] == k) {
        wasDown = true;
        break;
      }
    }
    if (!wasDown) onKeyPressed(k);
  }
  memcpy(prevKeys, keys, sizeof(prevKeys));
  Keypad.clear();
}

// The HID interfaces the keypad handler leaves. A wireless receiver can offer
// several keyboard interfaces, and the keypad's keys may arrive on one that
// the core's keyboard handler (one interface only) does not take. Each
// instance binds one leftover interface (the core allows one per handler),
// logs its first raw reports, and feeds boot-format keyboard reports
// ([modifiers, 0, 6 keys], optionally behind a report ID) from keyboard
// interfaces into onKeyPressed(), like the main handler. onReport() only
// queues; flush() runs from loop().
class RawHidInterface : public USBHostHIDDevice {
public:
  bool claim(uint8_t dev_addr, uint8_t idx, uint8_t protocol, const uint8_t *, uint16_t) override {
    if (!bindHidInterface(dev_addr, idx)) return false;  // already owns another interface
    _protocol = protocol;
    _logged = 0;
    memset(_prev, 0, sizeof(_prev));
    _r = _w;
    return true;
  }
  void onUnmount(uint8_t dev_addr, uint8_t idx) override {
    if (matches(dev_addr, idx)) clearHidInterfaceBinding();
  }
  void onReport(uint8_t, uint8_t, const uint8_t *report, uint16_t len) override {
    uint8_t w = _w, n = (uint8_t)((w + 1) % QUEUE);
    if (n == _r) return;  // queue full: drop
    Rep &q = _q[w];
    q.fullLen = len;
    q.len = len > sizeof(q.data) ? sizeof(q.data) : (uint8_t)len;
    memcpy(q.data, report, q.len);
    _w = n;
  }

  void flush() {
    while (_r != _w) {
      handle(_q[_r]);
      _r = (uint8_t)((_r + 1) % QUEUE);
    }
  }

private:
  struct Rep {
    uint8_t len;
    uint16_t fullLen;
    uint8_t data[16];
  };
  static const uint8_t QUEUE = 16, RAW_LOG_MAX = 40;
  Rep _q[QUEUE];
  volatile uint8_t _w = 0, _r = 0;
  uint8_t _protocol = 0, _logged = 0;
  uint8_t _prev[6] = {0};

  void handle(const Rep &q) {
    if (_logged < RAW_LOG_MAX) {  // diagnostics: which interface carries what
      _logged++;
      char hex[3 * sizeof(q.data) + 1];
      for (uint8_t i = 0; i < q.len; i++) snprintf(hex + 3 * i, 4, "%02x ", q.data[i]);
      hex[3 * q.len] = '\0';
      Log.printf("[usb] raw report: interface %u, %u bytes: %s\n", interfaceIndex(), q.fullLen, hex);
    }
    if (_protocol != HID_ITF_PROTOCOL_KEYBOARD) return;  // mouse etc.: log only
    const uint8_t *b = q.len == 8 ? q.data : q.len == 9 ? q.data + 1 : nullptr;
    if (b == nullptr || b[1] != 0) return;
    for (int i = 2; i < 8; i++) {
      if (b[i] > 0xE7) return;
    }
    for (int i = 2; i < 8; i++) {
      uint8_t k = b[i];
      if (k < 0x04) continue;  // 0 = empty, 1..3 = rollover/error codes
      bool wasDown = false;
      for (int j = 0; j < 6; j++) {
        if (_prev[j] == k) {
          wasDown = true;
          break;
        }
      }
      if (!wasDown) onKeyPressed(k);
    }
    memcpy(_prev, b + 2, 6);
  }
};
static RawHidInterface rawHid[2];

static void logKeypadMount() {
  static bool wasMounted = false;
  bool m = Keypad.mounted();
  if (m == wasMounted) return;
  wasMounted = m;
  memset(prevKeys, 0, sizeof(prevKeys));
  if (m) Keypad.requestLedsOff();
  Log.println(m ? "[usb] keypad attached" : "[usb] keypad detached");
}


// ---------------------------------------------------------------- door

// Calls openDoor() (secrets.h) once WiFi is up; on false, retries every
// OPEN_RETRY_MS until OPEN_GIVE_UP_MS after the code was typed.
static void serviceOpen() {
  if (!openPending) return;
  uint32_t now = millis();
  if (now - openRequestedMs > OPEN_GIVE_UP_MS) {
    Log.printf("[door] gave up: openDoor() kept failing for %lu s\n", (unsigned long)(OPEN_GIVE_UP_MS / 1000));
    openPending = false;
    return;
  }
  if ((int32_t)(now - nextOpenAttemptMs) < 0) return;
  if (WiFi.status() != WL_CONNECTED) {
    nextOpenAttemptMs = now + 200;  // wait for WiFi to come back
    return;
  }
  if (openDoor()) {
    Log.println("[door] opened");
    openPending = false;
  } else {
    Log.println("[door] openDoor() failed, retrying");
    nextOpenAttemptMs = millis() + OPEN_RETRY_MS;
  }
}

// ---------------------------------------------------------------- wifi

// Runs in the WiFi event task: only record, log from loop().
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wifiLastDisconnectReason = info.wifi_sta_disconnected.reason;
  }
}

static void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);  // no modem-sleep latency on the first packet after a code
  WiFi.setAutoReconnect(true);
  // Mesh networks have several APs with the same SSID. The default (fast scan) joins
  // the first one it hears; scan every channel and join the strongest instead.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  // Log what is on the air so the choice can be checked against it.
  int n = WiFi.scanNetworks(false, false, false, 120, 0, WIFI_SSID);
  Log.printf("[wifi] %d AP(s) with this SSID:\n", n < 0 ? 0 : n);
  for (int i = 0; i < n && i < 8; i++) {
    Log.printf("[wifi]   %s  rssi=%ld  ch=%ld\n", WiFi.BSSIDstr(i).c_str(), (long)WiFi.RSSI(i), (long)WiFi.channel(i));
  }
  WiFi.scanDelete();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void maintainWiFi() {
  static bool wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    wasConnected = connected;
    if (connected) {
      Log.printf("[wifi] connected, ip=%s rssi=%d bssid=%s ch=%ld\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                 WiFi.BSSIDstr().c_str(), (long)WiFi.channel());
    } else {
      Log.printf("[wifi] disconnected, reason=%d\n", wifiLastDisconnectReason);
    }
  }

  if (connected) {
    wifiDownSinceMs = 0;
    return;
  }
  uint32_t now = millis();
  if (wifiDownSinceMs == 0) {
    wifiDownSinceMs = now;
  } else if (now - wifiDownSinceMs > WIFI_KICK_MS) {
    Log.printf("[wifi] still down (last reason=%d), restarting connection\n", wifiLastDisconnectReason);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiDownSinceMs = now;
  }
}

// ---------------------------------------------------------------- main

static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software restart (e.g. after OTA)";
    case ESP_RST_PANIC:     return "CRASH (panic)";
    case ESP_RST_INT_WDT:   return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "CRASH (task watchdog)";
    case ESP_RST_WDT:       return "CRASH (other watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage dipped)";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_USB:       return "USB reset";
    case ESP_RST_JTAG:      return "JTAG reset";
    default:                return "unknown";
  }
}

// Diagnostic: the USB host port register shows what is on the wire before any
// enumeration: whether a device is seen, which data line it pulls high (a
// full-speed device pulls up D+, a low-speed one D-), and the detected speed.
// Read-only, so it does not disturb the USB stack.
// Logs at most once a second: the latest state and how many changes it saw.
static void logUsbPortChanges() {
  static uint32_t last = 0xFFFFFFFF, lastLogMs = 0, changes = 0;
  static usb_dwc_hprt_reg_t cur;
  usb_dwc_hprt_reg_t r;
  r.val = USB_DWC.hprt_reg.val;
  // Line state toggles with traffic once the port is enabled, so ignore it then.
  uint32_t key = r.prtconnsts | r.prtena << 1 | r.prtovrcurract << 2 | r.prtpwr << 3 | r.prtspd << 4 |
                 (r.prtena ? 0 : r.prtlnsts << 6);
  if (key != last) {
    last = key;
    cur = r;
    changes++;
  }
  if (!changes || millis() - lastLogMs < 1000) return;
  lastLogMs = millis();
  static const char *speed[] = {"high", "full", "low", "?"};
  Log.printf("[usb] port: connected=%u enabled=%u power=%u overcurrent=%u speed=%s D+=%u D-=%u (%lu change%s)\n",
             (unsigned)cur.prtconnsts, (unsigned)cur.prtena, (unsigned)cur.prtpwr, (unsigned)cur.prtovrcurract,
             speed[cur.prtspd], (unsigned)(cur.prtlnsts & 1), (unsigned)(cur.prtlnsts >> 1),
             (unsigned long)changes, changes == 1 ? "" : "s");
  changes = 0;
}

void setup() {
  rgbLedWriteOrdered(WINDOW_LED_PIN, WINDOW_LED_ORDER, 0, 0, 0);  // off until the window opens
  Serial.begin(115200);
  delay(500);
  esp_reset_reason_t rr = esp_reset_reason();
  Log.printf("\n[boot] door-keypad, reset reason: %s (%d)\n", resetReasonName(rr), (int)rr);

  WiFi.onEvent(onWiFiEvent);
  startWiFi();

  USBHostHID.addDevice(&hidLogger);  // first: sees every interface, claims none
  Keypad.registerWithHost();
  Keypad.setNotifyOnChangeOnly(true);
  Keypad.setLockHandling(false);  // lock keys never toggle the LEDs, so NumLock's light stays off
  Keypad.setReportCallback(onKeypadReport, nullptr);
  for (RawHidInterface &r : rawHid) USBHostHID.addDevice(&r);  // last: one leftover HID interface each
  if (!USBHost.begin()) {
    Log.println("[usb] USBHost.begin() failed");
  } else {
    Log.println("[usb] host ready, plug in the keypad");
  }
}

void loop() {
  USBHost.task();
  logUsbPortChanges();
  hidLogger.flush();
  for (RawHidInterface &r : rawHid) r.flush();
  logKeypadMount();
  if (Keypad.takeLedsOffSent()) Log.println("[usb] keypad LEDs set off");
  serviceRate();
  maintainWiFi();
  serviceNetWindow();
  serviceOpen();
  delay(2);
}
