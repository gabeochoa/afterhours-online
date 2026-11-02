#pragma once

// Optional test support for afterhours-online consumers.
//
// This header is deliberately separate from online.h: production games do
// not need to include it. It provides deterministic event pumping, event
// matching, a small sequenced test-message envelope, and a Transport
// decorator that can inject application-visible delay, loss, duplication,
// reordering, corruption, and blackouts above any backend.
//
// FaultTransport sits above the real transport. Dropping a send here
// therefore models an application packet that never entered the wire; it
// does not model UDP datagram loss underneath GNS's own retransmission.
// Use a UDP proxy or OS traffic shaping for that lower layer.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "core.h"
#include "serialize.h"

namespace afterhours::online::testing {

struct NamedEndpoint {
  std::string name;
  Transport *transport = nullptr;
};

struct RecordedEvent {
  std::string endpoint;
  Event event;
  std::chrono::milliseconds elapsed{0};
};

inline const Packet *packet_of(const RecordedEvent &recorded) {
  return std::get_if<Packet>(&recorded.event);
}

inline bool has_connected(const std::vector<RecordedEvent> &log,
                          std::string_view endpoint, PeerId peer) {
  for (const auto &recorded : log) {
    if (recorded.endpoint != endpoint)
      continue;
    if (const auto *event = std::get_if<Connected>(&recorded.event);
        event && event->peer == peer)
      return true;
  }
  return false;
}

inline bool has_disconnected(const std::vector<RecordedEvent> &log,
                             std::string_view endpoint, PeerId peer) {
  for (const auto &recorded : log) {
    if (recorded.endpoint != endpoint)
      continue;
    if (const auto *event = std::get_if<Disconnected>(&recorded.event);
        event && event->peer == peer)
      return true;
  }
  return false;
}

inline std::vector<Packet> packets_on(const std::vector<RecordedEvent> &log,
                                      std::string_view endpoint,
                                      std::optional<PeerId> from = std::nullopt) {
  std::vector<Packet> out;
  for (const auto &recorded : log) {
    if (recorded.endpoint != endpoint)
      continue;
    if (const auto *packet = packet_of(recorded);
        packet && (!from || packet->from == *from))
      out.push_back(*packet);
  }
  return out;
}

inline const Packet *find_packet(const std::vector<RecordedEvent> &log,
                                 std::string_view endpoint,
                                 std::string_view payload) {
  for (const auto &recorded : log) {
    if (recorded.endpoint != endpoint)
      continue;
    if (const auto *packet = packet_of(recorded);
        packet && view_bytes(packet->payload) == payload)
      return packet;
  }
  return nullptr;
}

inline std::string describe_event(const Event &event) {
  if (const auto *connected = std::get_if<Connected>(&event))
    return "connected peer=" + std::to_string(connected->peer);
  if (const auto *disconnected = std::get_if<Disconnected>(&event))
    return "disconnected peer=" + std::to_string(disconnected->peer);
  const auto &packet = std::get<Packet>(event);
  return "packet from=" + std::to_string(packet.from) +
         " bytes=" + std::to_string(packet.payload.size()) +
         " channel=" + std::to_string(static_cast<int>(packet.channel));
}

class EventPump {
public:
  explicit EventPump(std::vector<NamedEndpoint> endpoints)
      : endpoints_(std::move(endpoints)), started_(Clock::now()) {}

  std::size_t drain_once() {
    std::size_t drained = 0;
    for (auto &endpoint : endpoints_) {
      if (!endpoint.transport)
        continue;
      while (auto event = endpoint.transport->poll()) {
        log_.push_back(RecordedEvent{
            endpoint.name, std::move(*event),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - started_)});
        ++drained;
      }
    }
    return drained;
  }

  template <typename Predicate>
  bool pump_until(Predicate predicate, int timeout_ms, int poll_ms = 1) {
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    while (true) {
      drain_once();
      if (predicate(log_))
        return true;
      if (Clock::now() >= deadline)
        return false;
      std::this_thread::sleep_for(
          std::chrono::milliseconds(std::max(poll_ms, 0)));
    }
  }

  void pump_for(int duration_ms, int poll_ms = 1) {
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(std::max(duration_ms, 0));
    while (Clock::now() < deadline) {
      drain_once();
      std::this_thread::sleep_for(
          std::chrono::milliseconds(std::max(poll_ms, 0)));
    }
    drain_once();
  }

  [[nodiscard]] const std::vector<RecordedEvent> &log() const { return log_; }
  void clear_log() { log_.clear(); }
  void add_endpoint(NamedEndpoint endpoint) {
    endpoints_.push_back(std::move(endpoint));
  }
  void set_endpoints(std::vector<NamedEndpoint> endpoints) {
    endpoints_ = std::move(endpoints);
  }

private:
  using Clock = std::chrono::steady_clock;

  std::vector<NamedEndpoint> endpoints_;
  std::vector<RecordedEvent> log_;
  Clock::time_point started_;
};

// A tiny checksummed envelope for transport scenarios. Games can use the
// same pattern with their own packet types: sequence numbers make loss,
// duplication, and reordering observable instead of implicit.
inline constexpr std::uint32_t kSequenceMagic = 0xA110E2E1u;

inline std::uint8_t sequence_checksum(std::span<const std::byte> bytes) {
  std::uint32_t sum = 0;
  for (std::byte value : bytes)
    sum += std::to_integer<unsigned int>(value);
  return static_cast<std::uint8_t>(sum % 251u);
}

inline std::vector<std::byte> encode_sequence_packet(std::uint32_t sequence,
                                                     std::string_view body = {}) {
  Writer writer;
  writer.u32(kSequenceMagic);
  writer.u32(sequence);
  writer.string(body);
  writer.u8(sequence_checksum(writer.span()));
  return writer.take();
}

struct SequenceMessage {
  std::uint32_t sequence = 0;
  std::string body;
};

inline std::optional<SequenceMessage>
decode_sequence_packet(std::span<const std::byte> bytes) {
  if (bytes.empty() ||
      sequence_checksum(bytes.first(bytes.size() - 1)) !=
          std::to_integer<std::uint8_t>(bytes.back()))
    return std::nullopt;
  Reader reader(bytes);
  auto magic = reader.u32();
  auto sequence = reader.u32();
  auto body = reader.string();
  auto checksum = reader.u8();
  if (!magic || *magic != kSequenceMagic || !sequence || !body || !checksum ||
      !reader.exhausted())
    return std::nullopt;
  return SequenceMessage{*sequence, std::move(*body)};
}

class SequenceTracker {
public:
  enum class Observation { Accepted, Duplicate };

  Observation observe(std::uint32_t sequence) {
    if (seen_.contains(sequence)) {
      ++duplicates_;
      return Observation::Duplicate;
    }
    if (max_seen_ && sequence < *max_seen_)
      ++inversions_;
    if (!min_seen_ || sequence < *min_seen_)
      min_seen_ = sequence;
    if (!max_seen_ || sequence > *max_seen_)
      max_seen_ = sequence;
    seen_.insert(sequence);
    return Observation::Accepted;
  }

  [[nodiscard]] std::size_t unique_count() const { return seen_.size(); }
  [[nodiscard]] std::size_t duplicates() const { return duplicates_; }
  [[nodiscard]] std::size_t inversions() const { return inversions_; }
  [[nodiscard]] bool contains(std::uint32_t sequence) const {
    return seen_.contains(sequence);
  }
  [[nodiscard]] std::size_t missing_in_range() const {
    if (!min_seen_ || !max_seen_)
      return 0;
    return static_cast<std::size_t>(*max_seen_ - *min_seen_ + 1) - seen_.size();
  }

private:
  std::unordered_set<std::uint32_t> seen_;
  std::optional<std::uint32_t> min_seen_;
  std::optional<std::uint32_t> max_seen_;
  std::size_t duplicates_ = 0;
  std::size_t inversions_ = 0;
};

enum class Corruption : std::uint8_t {
  None,
  FlipByte,
  Truncate,
};

struct FaultConfig {
  std::uint32_t seed = 1;

  // Outgoing application-visible faults.
  int drop_first_sends = 0;
  double drop_send_rate = 0.0;
  bool dropped_send_reports_success = true;
  int duplicate_first_sends = 0;
  double duplicate_send_rate = 0.0;
  int corrupt_first_sends = 0;
  double corrupt_send_rate = 0.0;
  Corruption corruption = Corruption::None;
  std::size_t corrupt_offset = 0;
  std::size_t truncate_to_bytes = 1;
  std::optional<Channel> force_channel;

  // Incoming faults are applied as poll() drains the wrapped transport.
  // Delaying both endpoints models round-trip latency; delaying one models
  // an asymmetric path.
  double drop_receive_rate = 0.0;
  int latency_ms = 0;
  int jitter_ms = 0;
  int reorder_every = 0;      // 0 disables; every Nth subject gets extra delay.
  int reorder_extra_ms = 0;
  std::size_t max_queued_events = 0; // 0 means unlimited.
  bool apply_to_lifecycle_events = false;

  [[nodiscard]] bool is_clean() const {
    return drop_first_sends == 0 && drop_send_rate <= 0.0 &&
           duplicate_first_sends == 0 && duplicate_send_rate <= 0.0 &&
           corrupt_first_sends == 0 && corrupt_send_rate <= 0.0 &&
           corruption == Corruption::None && !force_channel &&
           drop_receive_rate <= 0.0 && latency_ms == 0 && jitter_ms == 0 &&
           reorder_every == 0 && reorder_extra_ms == 0 &&
           max_queued_events == 0;
  }
};

struct FaultStats {
  std::size_t send_calls = 0;
  std::size_t sends_dropped = 0;
  std::size_t sends_duplicated = 0;
  std::size_t sends_corrupted = 0;
  std::size_t broadcast_calls = 0;
  std::size_t broadcasts_dropped = 0;
  std::size_t events_received = 0;
  std::size_t events_dropped = 0;
  std::size_t events_delayed = 0;
};

class FaultTransport final : public Transport {
public:
  FaultTransport(std::unique_ptr<Transport> inner, FaultConfig config)
      : inner_(std::move(inner)), config_(config), rng_(config.seed) {}

  bool listen(std::uint16_t port) override {
    return inner_ && inner_->listen(port);
  }

  bool connect(std::string_view host, std::uint16_t port) override {
    return inner_ && inner_->connect(host, port);
  }

  void close() override {
    queued_.clear();
    if (inner_)
      inner_->close();
  }

  bool disconnect(PeerId peer) override {
    return inner_ && inner_->disconnect(peer);
  }

  bool send(PeerId to, std::span<const std::byte> payload,
            Channel channel) override {
    if (!inner_)
      return false;
    ++stats_.send_calls;
    const std::size_t ordinal = send_ordinal_++;
    if (blackout_sends_ || should_drop_send(ordinal)) {
      ++stats_.sends_dropped;
      return config_.dropped_send_reports_success;
    }

    std::vector<std::byte> owned;
    std::span<const std::byte> outgoing = payload;
    if (should_corrupt_send(ordinal)) {
      owned.assign(payload.begin(), payload.end());
      corrupt(owned);
      outgoing = owned;
      ++stats_.sends_corrupted;
    }

    const Channel effective_channel = config_.force_channel.value_or(channel);
    bool sent = inner_->send(to, outgoing, effective_channel);
    if (sent && should_duplicate_send(ordinal)) {
      ++stats_.sends_duplicated;
      sent = inner_->send(to, outgoing, effective_channel) || sent;
    }
    return sent;
  }

  bool broadcast(std::span<const std::byte> payload, Channel channel) override {
    if (!inner_)
      return false;
    ++stats_.broadcast_calls;
    const std::size_t ordinal = send_ordinal_++;
    if (blackout_sends_ || should_drop_send(ordinal)) {
      ++stats_.broadcasts_dropped;
      ++stats_.sends_dropped;
      return config_.dropped_send_reports_success;
    }

    std::vector<std::byte> owned;
    std::span<const std::byte> outgoing = payload;
    if (should_corrupt_send(ordinal)) {
      owned.assign(payload.begin(), payload.end());
      corrupt(owned);
      outgoing = owned;
      ++stats_.sends_corrupted;
    }

    const Channel effective_channel = config_.force_channel.value_or(channel);
    bool sent = inner_->broadcast(outgoing, effective_channel);
    if (sent && should_duplicate_send(ordinal)) {
      ++stats_.sends_duplicated;
      sent = inner_->broadcast(outgoing, effective_channel) || sent;
    }
    return sent;
  }

  std::optional<Event> poll() override {
    if (!inner_)
      return std::nullopt;
    drain_inner();
    if (queued_.empty())
      return std::nullopt;
    if (queued_.front().release > Clock::now())
      return std::nullopt;
    Event event = std::move(queued_.front().event);
    queued_.pop_front();
    return event;
  }

  void set_local_peer_id(PeerId peer) override {
    if (inner_)
      inner_->set_local_peer_id(peer);
  }

  [[nodiscard]] PeerId local_peer_id() const override {
    return inner_ ? inner_->local_peer_id() : kInvalidPeer;
  }
  [[nodiscard]] bool is_server() const override {
    return inner_ && inner_->is_server();
  }
  [[nodiscard]] bool is_open() const override {
    return inner_ && inner_->is_open();
  }

  void set_blackout_sends(bool enabled) { blackout_sends_ = enabled; }
  void set_blackout_receives(bool enabled) { blackout_receives_ = enabled; }
  void clear_queued_events() { queued_.clear(); }
  [[nodiscard]] const FaultConfig &config() const { return config_; }
  [[nodiscard]] FaultConfig &config() { return config_; }
  [[nodiscard]] const FaultStats &stats() const { return stats_; }
  [[nodiscard]] Transport *inner() { return inner_.get(); }

private:
  using Clock = std::chrono::steady_clock;

  struct QueuedEvent {
    Clock::time_point release;
    std::uint64_t ordinal = 0;
    Event event;
  };

  bool chance(double rate) {
    if (rate <= 0.0)
      return false;
    if (rate >= 1.0)
      return true;
    return std::bernoulli_distribution(rate)(rng_);
  }

  bool should_drop_send(std::size_t ordinal) {
    return ordinal < static_cast<std::size_t>(std::max(config_.drop_first_sends, 0)) ||
           chance(config_.drop_send_rate);
  }

  bool should_duplicate_send(std::size_t ordinal) {
    return ordinal <
               static_cast<std::size_t>(std::max(config_.duplicate_first_sends, 0)) ||
           chance(config_.duplicate_send_rate);
  }

  bool should_corrupt_send(std::size_t ordinal) {
    if (config_.corruption == Corruption::None)
      return false;
    return ordinal <
               static_cast<std::size_t>(std::max(config_.corrupt_first_sends, 0)) ||
           chance(config_.corrupt_send_rate);
  }

  void corrupt(std::vector<std::byte> &payload) {
    if (payload.empty())
      return;
    if (config_.corruption == Corruption::Truncate) {
      payload.resize(std::min(payload.size(), config_.truncate_to_bytes));
      return;
    }
    const std::size_t offset = std::min(config_.corrupt_offset, payload.size() - 1);
    payload[offset] = static_cast<std::byte>(
        std::to_integer<unsigned char>(payload[offset]) ^ 0xFFu);
  }

  bool faults_apply(const Event &event) const {
    return config_.apply_to_lifecycle_events ||
           std::holds_alternative<Packet>(event);
  }

  int incoming_delay_ms(std::uint64_t ordinal) {
    int delay = std::max(config_.latency_ms, 0);
    if (config_.jitter_ms > 0) {
      delay += std::uniform_int_distribution<int>(0, config_.jitter_ms)(rng_);
    }
    if (config_.reorder_every > 0 &&
        ordinal % static_cast<std::uint64_t>(config_.reorder_every) == 0)
      delay += std::max(config_.reorder_extra_ms, 0);
    return delay;
  }

  void drain_inner() {
    while (auto event = inner_->poll()) {
      ++stats_.events_received;
      const bool subject = faults_apply(*event);
      if (subject && (blackout_receives_ || chance(config_.drop_receive_rate))) {
        ++stats_.events_dropped;
        continue;
      }
      if (subject && config_.max_queued_events > 0 &&
          queued_.size() >= config_.max_queued_events) {
        ++stats_.events_dropped;
        continue;
      }

      const std::uint64_t ordinal = receive_ordinal_++;
      const int delay_ms = subject ? incoming_delay_ms(ordinal) : 0;
      if (delay_ms > 0)
        ++stats_.events_delayed;
      queued_.push_back(QueuedEvent{
          Clock::now() + std::chrono::milliseconds(delay_ms), ordinal,
          std::move(*event)});
    }
    std::stable_sort(queued_.begin(), queued_.end(),
                     [](const QueuedEvent &a, const QueuedEvent &b) {
                       if (a.release != b.release)
                         return a.release < b.release;
                       return a.ordinal < b.ordinal;
                     });
  }

  std::unique_ptr<Transport> inner_;
  FaultConfig config_;
  FaultStats stats_;
  std::mt19937 rng_;
  std::deque<QueuedEvent> queued_;
  std::size_t send_ordinal_ = 0;
  std::uint64_t receive_ordinal_ = 0;
  bool blackout_sends_ = false;
  bool blackout_receives_ = false;
};

inline std::unique_ptr<Transport> maybe_wrap_faults(
    std::unique_ptr<Transport> transport, const FaultConfig &config) {
  if (config.is_clean())
    return transport;
  return std::make_unique<FaultTransport>(std::move(transport), config);
}

} // namespace afterhours::online::testing
