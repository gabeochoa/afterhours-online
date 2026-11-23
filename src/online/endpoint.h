#pragma once

// Endpoint parsing, connect-attempt UX, per-peer RTT, and the
// unreliable-input edge latch (OG-20..OG-23, was Pumpkin ON-3..ON-6;
// endpoint contract half of OG-06 / gaps item 4).
//
// Endpoint contract, stated once (games: do not restate it differently):
// parse_endpoint validates SYNTAX only. It accepts IPv4 literals,
// bracketed IPv6 literals, and DNS-shaped names. This library does
// NOT resolve DNS today: backends hand the host to their address
// parser, which accepts IP literals, so a DNS name parses here and
// then fails at connect() as a bad address. That mismatch is OG-06's
// known state, made explicit at this one boundary instead of being
// split between a game's text field and a backend's error string.
// Hosts that need DNS must resolve before connect() until a resolver
// lands in a backend.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core.h"

namespace afterhours::online {

struct Endpoint {
  std::string host; // IP literal or DNS-shaped name, brackets stripped
  std::uint16_t port = 0;
  bool ipv6_literal = false;
};

namespace detail {
[[nodiscard]] inline std::string_view trim_endpoint(std::string_view v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
    v.remove_prefix(1);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
    v.remove_suffix(1);
  return v;
}
[[nodiscard]] inline std::optional<std::uint16_t>
parse_port(std::string_view text) {
  if (text.empty())
    return std::nullopt;
  int port = 0;
  for (const char c : text) {
    if (c < '0' || c > '9')
      return std::nullopt;
    port = port * 10 + (c - '0');
    if (port > 65535)
      return std::nullopt;
  }
  if (port == 0)
    return std::nullopt;
  return static_cast<std::uint16_t>(port);
}
[[nodiscard]] inline bool host_char_ok(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}
[[nodiscard]] inline bool ipv6_char_ok(char c) {
  return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
         (c >= '0' && c <= '9') || c == ':' || c == '.' || c == '%';
}
} // namespace detail

// Parse "host", "host:port", "[ipv6]", or "[ipv6]:port". The port
// falls back to `default_port` when absent.
[[nodiscard]] inline std::optional<Endpoint>
parse_endpoint(std::string_view text, std::uint16_t default_port) {
  text = detail::trim_endpoint(text);
  if (text.empty())
    return std::nullopt;
  Endpoint out;
  out.port = default_port;
  if (text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string_view::npos)
      return std::nullopt;
    const std::string_view host = text.substr(1, close - 1);
    if (host.empty())
      return std::nullopt;
    for (const char c : host)
      if (!detail::ipv6_char_ok(c))
        return std::nullopt;
    const std::string_view rest = detail::trim_endpoint(text.substr(close + 1));
    if (!rest.empty()) {
      if (rest.front() != ':')
        return std::nullopt;
      const auto port = detail::parse_port(detail::trim_endpoint(rest.substr(1)));
      if (!port)
        return std::nullopt;
      out.port = *port;
    }
    out.host = std::string(host);
    out.ipv6_literal = true;
    return out;
  }
  std::string_view host = text;
  if (const std::size_t colon = text.rfind(':');
      colon != std::string_view::npos) {
    if (text.find(':') != colon)
      return std::nullopt; // unbracketed IPv6 or junk: reject, don't guess
    host = detail::trim_endpoint(text.substr(0, colon));
    const auto port =
        detail::parse_port(detail::trim_endpoint(text.substr(colon + 1)));
    if (!port)
      return std::nullopt;
    out.port = *port;
  }
  if (host.empty())
    return std::nullopt;
  for (const char c : host)
    if (!detail::host_char_ok(c))
      return std::nullopt;
  out.host = std::string(host);
  return out;
}

[[nodiscard]] inline std::string format_endpoint(const Endpoint &endpoint) {
  if (endpoint.ipv6_literal)
    return "[" + endpoint.host + "]:" + std::to_string(endpoint.port);
  return endpoint.host + ":" + std::to_string(endpoint.port);
}

// Human status for a terminal disconnect, shared so every client
// spells "server full / unreachable / timed out" the same way.
[[nodiscard]] inline std::string_view
connect_failure_status(DisconnectReason reason, bool hello_sent) {
  switch (reason) {
  case DisconnectReason::Rejected:
    return "Connect failed: server full";
  case DisconnectReason::Unreachable:
    return "Connect failed: server unreachable";
  case DisconnectReason::LocalProblem:
    return "Connect failed: local network problem";
  case DisconnectReason::ClosedByPeer:
    return hello_sent ? "Connect failed: server closed connection"
                      : "Connect failed: server unreachable";
  case DisconnectReason::LocalClose:
    return "Connect failed: connection closed";
  case DisconnectReason::Unknown:
    return "Connect failed";
  }
  return "Connect failed";
}

// Client connect-attempt state (OG-22): hello-sent/welcomed flags, a
// welcome deadline, and terminal-reason mapping. Without it a client
// says "Connecting" forever (OG-01's original complaint). Time is
// injectable for tests; games pass steady_clock::now().
class ConnectAttempt {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  static constexpr std::chrono::seconds DEFAULT_DEADLINE{12};

  explicit ConnectAttempt(
      std::chrono::nanoseconds deadline = DEFAULT_DEADLINE)
      : deadline(deadline) {}

  void begin(TimePoint now) {
    in_progress = true;
    hello_dispatched = false;
    welcome_received = false;
    started = now;
  }
  void reset() { in_progress = false; hello_dispatched = welcome_received = false; }
  void note_hello_sent() { hello_dispatched = true; }
  void note_welcomed() { welcome_received = true; }

  [[nodiscard]] bool active() const { return in_progress; }
  [[nodiscard]] bool hello_sent() const { return hello_dispatched; }
  [[nodiscard]] bool welcomed() const { return welcome_received; }

  // Terminal disconnect while connecting: ends the attempt and
  // returns the status text. nullopt once welcomed (the disconnect is
  // a mid-session event, not a connect failure).
  [[nodiscard]] std::optional<std::string_view>
  note_disconnected(DisconnectReason reason) {
    if (!in_progress || welcome_received)
      return std::nullopt;
    in_progress = false;
    return connect_failure_status(reason, hello_dispatched);
  }

  // Deadline check; on expiry ends the attempt and returns status.
  [[nodiscard]] std::optional<std::string_view> poll(TimePoint now) {
    if (!in_progress || welcome_received || now - started < deadline)
      return std::nullopt;
    in_progress = false;
    return std::string_view{hello_dispatched ? "Connect failed: no welcome from server"
                                        : "Connect failed: server unreachable"};
  }

private:
  std::chrono::nanoseconds deadline;
  TimePoint started{};
  bool in_progress = false;
  bool hello_dispatched = false;
  bool welcome_received = false;
};

// Per-peer round-trip tracker (OG-20, extends OG-07). Outstanding
// probes are keyed by (peer, seq); RTT is 75/25 smoothed per peer and
// samples go stale instead of living forever.
class RttTracker {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  static constexpr std::chrono::seconds DEFAULT_STALE_AFTER{4};

  void note_ping(PeerId peer, std::uint32_t seq, TimePoint now) {
    auto &outstanding = pending[peer];
    outstanding[seq] = now;
    while (outstanding.size() > 8)
      outstanding.erase(outstanding.begin());
  }

  // Record a Pong; returns the raw sample in ms when (peer, seq) was
  // outstanding. The first Pong wins; duplicates are ignored.
  [[nodiscard]] std::optional<float> note_pong(PeerId peer, std::uint32_t seq,
                                               TimePoint now) {
    const auto pit = pending.find(peer);
    if (pit == pending.end())
      return std::nullopt;
    const auto sit = pit->second.find(seq);
    if (sit == pit->second.end())
      return std::nullopt;
    const float sample = std::chrono::duration<float, std::milli>(now - sit->second).count();
    pit->second.erase(sit);
    PeerState &state = peers[peer];
    state.rtt_ms = state.rtt_ms <= 0.f ? sample : 0.75f * state.rtt_ms + 0.25f * sample;
    state.last_sample = now;
    state.has_sample = true;
    return sample;
  }

  // Smoothed RTT for one peer, or nullopt with no fresh sample.
  [[nodiscard]] std::optional<float>
  rtt_ms(PeerId peer, TimePoint now,
         std::chrono::nanoseconds stale_after = DEFAULT_STALE_AFTER) const {
    const auto it = peers.find(peer);
    if (it == peers.end() || !it->second.has_sample ||
        now - it->second.last_sample > stale_after)
      return std::nullopt;
    return it->second.rtt_ms;
  }

  void forget_peer(PeerId peer) {
    pending.erase(peer);
    peers.erase(peer);
  }
  void clear() { pending.clear(); peers.clear(); }

private:
  struct PeerState {
    float rtt_ms = 0.f;
    TimePoint last_sample{};
    bool has_sample = false;
  };
  std::map<PeerId, std::map<std::uint32_t, TimePoint>> pending;
  std::map<PeerId, PeerState> peers;
};

// OR-latch for one-frame edges on unreliable inputs (OG-23): a jump
// press latches until a sim tick consumes it, so one dropped packet
// cannot eat the press. Sender side: attach the latched state to each
// outgoing input; receiver side: latch on receipt, consume per tick.
class EdgeLatch {
public:
  void latch(bool pressed) { latched = latched || pressed; }
  [[nodiscard]] bool peek() const { return latched; }
  [[nodiscard]] bool consume() {
    const bool value = latched;
    latched = false;
    return value;
  }
  void clear() { latched = false; }

private:
  bool latched = false;
};

} // namespace afterhours::online
