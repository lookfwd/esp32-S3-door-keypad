#pragma once

// Copy this file to secrets.h (same folder) and fill in your values.
// secrets.h is git-ignored; this example is not, so keep real values out of it.
// The sketch includes it after its Log object, so openDoor() can log.

#define WIFI_SSID     "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// OTA upload password. Use a long random one, e.g.:
//   LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 20
#define OTA_PASSWORD  "change-me-to-a-long-random-string"

// Keypad code that opens the door: digits only, up to 15. Use at least 6.
static const char *CODE = "123456";

// Called when CODE is typed: do whatever opens your door, as you see fit, and
// return true once it worked. On false it is retried every second for up to
// 15 s. Log works like Serial and also reaches the telnet log.
//
// Example: call an HTTP endpoint of your door opener. Protect that endpoint
// (authentication or a separate network), or anyone on the WiFi can call it
// directly and skip the keypad.
static bool openDoor() {
  String url = "http://192.0.2.20/open-door";  // your door opener's REST endpoint
  HTTPClient http;
  if (!http.begin(url)) {
    Log.println("bad URL");
    return false;
  }
  uint32_t t0 = millis();
  int status = http.GET();
  String body = status > 0 ? http.getString() : HTTPClient::errorToString(status);
  http.end();
  Log.printf("GET %s -> %d %s (%lu ms)\n", url.c_str(), status, body.c_str(), (unsigned long)(millis() - t0));
  return status == 200;
}
