#include <cstdio>
#include <cstdlib>
#include "KeyRateLimiter.h"

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);              \
      failures++;                                                      \
    }                                                                  \
  } while (0)

// As in the sketch: accept while the bucket holds <= 30, cap 100, one drains every 2 s.
static const int MAX_KEYS = 30;
static const int CAP_KEYS = 100;
static const uint32_t DRAIN_MS = 2000;
static KeyRateLimiter make() { return KeyRateLimiter(MAX_KEYS, CAP_KEYS, DRAIN_MS); }

// Simulates loop(): tick every 2 ms between keystrokes, as the sketch does.
static void tickUntil(KeyRateLimiter &l, uint32_t &t, uint32_t until) {
  while (t != until) {
    t += 2;
    if ((int32_t)(t - until) > 0) t = until;
    l.tick(t);
  }
}

static void burstAndFlood(uint32_t t0) {
  KeyRateLimiter l = make();
  uint32_t t = t0;
  l.tick(t);
  for (int i = 0; i < MAX_KEYS; i++) {
    CHECK(l.onKey(t) == KeyRateLimiter::ACCEPT);
    t += 1;
  }
  CHECK(l.onKey(t) == KeyRateLimiter::DROP);  // the 31st
  // Keep flooding: everything dropped, and the counter stops at the cap.
  int dropped = 0;
  for (int i = 0; i < 1000; i++) {
    t += 1;
    if (l.onKey(t) == KeyRateLimiter::DROP) dropped++;
  }
  CHECK(dropped == 1000);
  CHECK(l.level() > CAP_KEYS - 0.01f && l.level() <= CAP_KEYS);
  // Stop. Probe copies so the probe keystroke doesn't change the real bucket.
  uint32_t stop = t;
  KeyRateLimiter a = l; uint32_t ta = t;
  tickUntil(a, ta, stop + 141900);            // 100 -> 29.05: one key would make 30.05
  CHECK(a.dropping());
  CHECK(a.onKey(ta) == KeyRateLimiter::DROP);
  KeyRateLimiter b = l; uint32_t tb = t;
  tickUntil(b, tb, stop + 142100);            // 100 -> 28.95
  CHECK(!b.dropping());
  CHECK(b.onKey(tb) == KeyRateLimiter::ACCEPT);
  CHECK(b.onKey(tb) == KeyRateLimiter::DROP);  // only one: it is back at the edge
  std::printf("  flood of 1031 keys: bucket %.2f/%d; next key accepted after ~142 s\n", l.level(), CAP_KEYS);
}

static void exactBoundary() {
  // All in the same millisecond (no drain): exactly MAX_KEYS accepted, the next dropped.
  KeyRateLimiter l = make();
  uint32_t t = 7000;
  l.tick(t);
  for (int i = 0; i < MAX_KEYS; i++) CHECK(l.onKey(t) == KeyRateLimiter::ACCEPT);
  CHECK(l.dropping());
  CHECK(l.onKey(t) == KeyRateLimiter::DROP);
}

static void steadyPaces() {
  {  // exactly one key per 2 s never drops
    KeyRateLimiter l = make();
    uint32_t t = 5000;
    for (int i = 0; i < 2000; i++) {
      CHECK(l.onKey(t) == KeyRateLimiter::ACCEPT);
      tickUntil(l, t, t + DRAIN_MS);
    }
  }
  {  // one key per 1.9 s fills slowly, then never gets through again
    KeyRateLimiter l = make();
    uint32_t t = 5000;
    int accepted = 0;
    while (l.onKey(t) == KeyRateLimiter::ACCEPT && accepted < 10000) {
      accepted++;
      tickUntil(l, t, t + 1900);
    }
    CHECK(accepted < 10000);
    int later = 0;
    for (int i = 0; i < 500; i++) {
      tickUntil(l, t, t + 1900);
      if (l.onKey(t) == KeyRateLimiter::ACCEPT) later++;
    }
    CHECK(later == 0);
    std::printf("  1 key/1.9 s: %d accepted (%.0f s), then 0 of the next 500\n", accepted, accepted * 1.9);
  }
}

static void attackerKeepsFiring() {
  // After a flood, one key per second forever: nothing gets through.
  KeyRateLimiter l = make();
  uint32_t t = 1000;
  for (int i = 0; i < 200; i++) l.onKey(t++);
  int accepted = 0;
  for (int i = 0; i < 3600; i++) {
    tickUntil(l, t, t + 1000);
    if (l.onKey(t) == KeyRateLimiter::ACCEPT) accepted++;
  }
  CHECK(accepted == 0);
  std::printf("  1 key/s for an hour after a flood: %d accepted, bucket %.1f\n", accepted, l.level());
}

static void normalUse() {
  // Fast normal typing: 30 keys at 150 ms are all accepted.
  KeyRateLimiter l = make();
  uint32_t t = 1000;
  for (int i = 0; i < 30; i++) {
    CHECK(l.onKey(t) == KeyRateLimiter::ACCEPT);
    tickUntil(l, t, t + 150);
  }
  std::printf("  30 keys at 150 ms -> bucket %.2f/%d\n", l.level(), MAX_KEYS);
}

int main() {
  std::printf("burst and flood near 0\n");
  burstAndFlood(1000);
  std::printf("burst and flood across millis() wraparound\n");
  burstAndFlood(0xFFFFFFFFu - 60000u);
  std::printf("exact boundary\n");
  exactBoundary();
  std::printf("steady paces\n");
  steadyPaces();
  std::printf("attacker keeps firing\n");
  attackerKeepsFiring();
  std::printf("normal use\n");
  normalUse();
  std::printf(failures ? "\n%d FAILURES\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
