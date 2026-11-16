#pragma once

#include <stdint.h>

/*
 * Leaky-bucket keystroke limiter, no lockout.
 *
 * Every keystroke adds one to the bucket, even a dropped one, up to capKeys.
 * The bucket leaks one keystroke every drainMs. A keystroke is accepted only
 * while the bucket, including that keystroke, holds at most maxKeys; above
 * that it is dropped (silently: it never reaches the code entry).
 *
 * With maxKeys 30, capKeys 100, drainMs 2000:
 *   - up to 30 keys in a burst are accepted, then one every 2 s;
 *   - once the bucket is above 30, typing faster than one key per 2 s gets
 *     every key dropped;
 *   - after a flood the bucket sits at 100 and needs (100 - 29) * 2 s = 142 s
 *     before the next key is accepted again.
 *
 * The level is kept in milliseconds of drain time (one keystroke = drainMs,
 * drains 1 per ms), so it is all integer math. Times are millis() values;
 * unsigned subtraction keeps it correct across the 49.7-day wraparound as
 * long as tick() runs regularly.
 */
class KeyRateLimiter {
public:
  enum Result { ACCEPT, DROP };

  KeyRateLimiter(uint32_t maxKeys, uint32_t capKeys, uint32_t drainMs)
    : _msPerKey(drainMs), _limit(drainMs * maxKeys), _cap(drainMs * capKeys) {}

  /** Call once per keystroke. */
  Result onKey(uint32_t now) {
    tick(now);
    _level = (_cap - _level < _msPerKey) ? _cap : _level + _msPerKey;
    return _level <= _limit ? ACCEPT : DROP;
  }

  /** Drain the bucket. */
  void tick(uint32_t now) {
    uint32_t elapsed = now - _last;
    _last = now;
    _level = (elapsed >= _level) ? 0 : _level - elapsed;
  }

  /** True while the next keystroke would be dropped. */
  bool dropping() const {
    return _level + _msPerKey > _limit;
  }

  /** Keystrokes currently in the bucket (fractional while draining). */
  float level() const {
    return (float)_level / (float)_msPerKey;
  }

private:
  uint32_t _msPerKey;
  uint32_t _limit;
  uint32_t _cap;
  uint32_t _level = 0;
  uint32_t _last = 0;
};
