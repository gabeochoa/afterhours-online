#pragma once

// Snapshot interpolation buffer and wall-clock send pacing (OG-05 /
// gaps item 6, Pumpkin Q, Pharmasea G-P2).
//
// Both snapshot games hand-rolled the same two pieces: a bounded deque
// of authoritative snapshots sampled a fixed delay behind the newest,
// and steady_clock pacing for input/snapshot/probe sends (wall clock,
// not game dt: a fixed-dt or headless loop can spin far faster than
// real time and would otherwise flood the connection). These headers
// own only the timing and buffer bookkeeping. Snapshot schemas, which
// fields lerp, prediction, reconciliation thresholds, and authority
// stay in the game.
//
// Sampling policy, spelled out because games must not rediscover it:
// - No extrapolation. A target past the newest snapshot returns the
//   newest; a target before the oldest returns the oldest.
// - Pruning happens on sample: entries older than the bracketing pair
//   for the sampled target are dropped, so the buffer stays small
//   without a separate housekeeping call.
// - Stale (tick older than the newest) and duplicate (tick equal to
//   the newest) pushes are rejected and counted, not silently stored.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace afterhours::online {

// Paces a recurring send at a fixed wall-clock period. The clock is
// injectable: pass explicit time points in tests, omit them in games.
class RateGate {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit RateGate(std::chrono::nanoseconds period) : period_(period) {}

  // True (and records the send) at most once per period. The first
  // call after construction or reset() always succeeds, matching the
  // games' zero-initialized last-send timestamps.
  [[nodiscard]] bool try_take(TimePoint now) {
    if (has_last_ && now - last_ < period_)
      return false;
    last_ = now;
    has_last_ = true;
    return true;
  }
  [[nodiscard]] bool try_take() { return try_take(Clock::now()); }

  // Forget the last send; the next try_take succeeds immediately.
  void reset() { has_last_ = false; }

  void set_period(std::chrono::nanoseconds period) { period_ = period; }
  [[nodiscard]] std::chrono::nanoseconds period() const { return period_; }

  // Time until the next allowed send (zero when one is due now).
  [[nodiscard]] std::chrono::nanoseconds remaining(TimePoint now) const {
    if (!has_last_)
      return std::chrono::nanoseconds{0};
    const auto elapsed = now - last_;
    return elapsed >= period_ ? std::chrono::nanoseconds{0}
                              : period_ - elapsed;
  }

private:
  std::chrono::nanoseconds period_;
  TimePoint last_{};
  bool has_last_ = false;
};

// Convenience constructors for the rates games actually spell.
[[nodiscard]] inline RateGate gate_for_hz(double hz) {
  const auto period = hz > 0.0
                          ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::duration<double>(1.0 / hz))
                          : std::chrono::nanoseconds{0};
  return RateGate{period};
}

// Bounded buffer of authoritative snapshots, keyed by tick.
//
// T is the game's snapshot value type. Interpolation is the game's:
// pass a callable `T(const T& a, const T& b, float t)` that lerps the
// fields the game interpolates and takes the rest from `b` (the newer
// snapshot), exactly as Pumpkin Beachball's sampler does today.
template <typename T> class SnapshotBuffer {
public:
  struct Entry {
    std::uint64_t tick = 0;
    T value;
  };

  explicit SnapshotBuffer(std::size_t capacity = 32) : capacity_(capacity) {}

  // Store a snapshot. Returns false (and counts the reason) for a
  // stale or duplicate tick; the buffer is left unchanged.
  bool push(std::uint64_t tick, T value) {
    if (!entries_.empty()) {
      if (tick < entries_.back().tick) {
        ++stale_;
        return false;
      }
      if (tick == entries_.back().tick) {
        ++duplicates_;
        return false;
      }
    }
    entries_.push_back(Entry{tick, std::move(value)});
    ++accepted_;
    while (entries_.size() > capacity_)
      entries_.pop_front();
    return true;
  }

  // Sample at an absolute tick, interpolating between the bracketing
  // snapshots. Entries before the bracketing pair are pruned.
  template <typename Lerp>
  [[nodiscard]] std::optional<T> sample(std::uint64_t target, Lerp &&lerp) {
    if (entries_.empty())
      return std::nullopt;
    while (entries_.size() > 2 && entries_[1].tick <= target)
      entries_.pop_front();
    if (entries_.size() == 1 || entries_.front().tick >= target)
      return entries_.front().value;
    if (target >= entries_.back().tick)
      return entries_.back().value;
    // Find the bracketing pair (buffer is tick-ordered and small).
    std::size_t i = 0;
    while (i + 1 < entries_.size() && entries_[i + 1].tick < target)
      ++i;
    const Entry &a = entries_[i];
    const Entry &b = entries_[i + 1];
    const float span =
        static_cast<float>(std::max<std::uint64_t>(b.tick - a.tick, 1));
    const float t =
        std::clamp(static_cast<float>(target - a.tick) / span, 0.f, 1.f);
    return lerp(a.value, b.value, t);
  }

  // Sample `delay_ticks` behind the newest snapshot: the fixed-delay
  // interpolation both games use (Pumpkin: six ticks at 20 Hz).
  template <typename Lerp>
  [[nodiscard]] std::optional<T> sample_delayed(std::uint64_t delay_ticks,
                                                Lerp &&lerp) {
    if (entries_.empty())
      return std::nullopt;
    const std::uint64_t newest = entries_.back().tick;
    const std::uint64_t target =
        newest > delay_ticks ? newest - delay_ticks : entries_.front().tick;
    return sample(target, std::forward<Lerp>(lerp));
  }

  void clear() { entries_.clear(); }

  // Restart interpolation around the freshest snapshot (Pumpkin's
  // serve reset: stale bracketing would smear entities across a
  // teleport). No-op when empty.
  void clear_keeping_newest() {
    if (entries_.empty())
      return;
    Entry newest = std::move(entries_.back());
    entries_.clear();
    entries_.push_back(std::move(newest));
  }

  [[nodiscard]] bool empty() const { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  [[nodiscard]] const T *newest() const {
    return entries_.empty() ? nullptr : &entries_.back().value;
  }
  [[nodiscard]] const T *oldest() const {
    return entries_.empty() ? nullptr : &entries_.front().value;
  }
  [[nodiscard]] std::uint64_t newest_tick() const {
    return entries_.empty() ? 0 : entries_.back().tick;
  }

  // Push accounting, for HUDs/logs and for spotting a sender that
  // stopped stamping monotonic ticks.
  [[nodiscard]] std::uint64_t accepted() const { return accepted_; }
  [[nodiscard]] std::uint64_t duplicates() const { return duplicates_; }
  [[nodiscard]] std::uint64_t stale_dropped() const { return stale_; }

private:
  std::deque<Entry> entries_;
  std::size_t capacity_;
  std::uint64_t accepted_ = 0;
  std::uint64_t duplicates_ = 0;
  std::uint64_t stale_ = 0;
};

} // namespace afterhours::online
