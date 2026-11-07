// Configurable end-to-end contract tests for afterhours-online.
//
// The same scenarios run against LocalTransport and, when compiled with
// ONLINE_E2E_WITH_GNS, GnsTransport over real UDP loopback. Backend
// differences are explicit BackendCaps, not scattered #ifdefs: a scenario
// can assert the portable contract, record a backend-defined behavior, or
// mark a known gap without aborting the whole run.
//
// Run with --help for the full option list. The game-level multi-process
// harness lives in pumpkin-beachball/tests/online_e2e.py.

#include "../src/online/online.h"
#include "../src/online/testing.h"
#ifdef ONLINE_E2E_WITH_GNS
#include "../src/online/gns.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

using namespace afterhours::online;
namespace online_testing = afterhours::online::testing;
using online_testing::EventPump;
using online_testing::FaultConfig;
using online_testing::FaultTransport;
using online_testing::NamedEndpoint;
using online_testing::RecordedEvent;
using online_testing::SequenceTracker;

enum class BackendChoice { Local, Gns, Both };

struct Options {
#ifdef ONLINE_E2E_WITH_GNS
  BackendChoice backend = BackendChoice::Both;
#else
  BackendChoice backend = BackendChoice::Local;
#endif
  std::vector<std::string> scenario_names;
  std::string filter;
  bool list = false;
  bool fail_fast = false;
  bool strict_gaps = false;
  bool json = false;
  bool verbose = false;
  int repeat = 1;
  int timeout_ms = 5000;
  int poll_ms = 1;
  int clients = 2;
  int flood_count = 250;
  int payload_max = 65536;
  std::uint32_t seed = 0xA110u;
  std::string host = "127.0.0.1";
  std::uint16_t port_base = 28100;
  std::string fault_profile = "clean";
  FaultConfig profile_faults;
};

struct AbortScenario {};
struct SkipScenario {
  std::string reason;
};

struct TestContext {
  const Options &options;
  std::string backend;
  std::string scenario;
  std::vector<std::string> failures;
  std::vector<std::string> notes;
  std::vector<std::string> gaps;

  void expect(bool condition, std::string message) {
    if (!condition)
      failures.push_back(std::move(message));
  }

  void require(bool condition, std::string message) {
    if (!condition) {
      failures.push_back(std::move(message));
      throw AbortScenario{};
    }
  }

  void note(std::string message) { notes.push_back(std::move(message)); }

  // Record a behavior that should eventually hold. The run stays green
  // unless --strict-gaps is passed.
  void gap(bool desired, std::string message) {
    if (!desired)
      gaps.push_back(std::move(message));
  }

  [[noreturn]] void skip(std::string reason) {
    throw SkipScenario{std::move(reason)};
  }
};

struct BackendCaps {
  bool client_id_on_connect = false;
  bool synchronous_connect_rejection = false;
  bool direct_client_to_client = false;
  bool client_broadcast_to_clients = false;
  bool exact_no_delay_label = false;
  bool terminal_event_on_unreachable = false;
  std::size_t max_clients = std::numeric_limits<std::size_t>::max();
};

class Driver {
public:
  virtual ~Driver() = default;
  [[nodiscard]] virtual std::string_view name() const = 0;
  [[nodiscard]] virtual BackendCaps caps() const = 0;
  [[nodiscard]] virtual std::uint16_t bound_port() const = 0;
  virtual std::unique_ptr<Transport> make_endpoint(FaultConfig faults = {}) = 0;
  virtual bool listen(Transport &transport) = 0;
  virtual bool connect(Transport &transport) = 0;
  virtual std::uint16_t unused_port() = 0;
};

class LocalDriver final : public Driver {
public:
  [[nodiscard]] std::string_view name() const override { return "local"; }

  [[nodiscard]] BackendCaps caps() const override {
    BackendCaps caps;
    caps.client_id_on_connect = true;
    caps.synchronous_connect_rejection = true;
    caps.direct_client_to_client = true;
    caps.client_broadcast_to_clients = true;
    caps.exact_no_delay_label = true;
    caps.terminal_event_on_unreachable = true;
    return caps;
  }

  [[nodiscard]] std::uint16_t bound_port() const override { return 7777; }

  std::unique_ptr<Transport> make_endpoint(FaultConfig faults = {}) override {
    return online_testing::maybe_wrap_faults(
        std::make_unique<LocalTransport>(hub_), faults);
  }

  bool listen(Transport &transport) override { return transport.listen(7777); }

  bool connect(Transport &transport) override {
    return transport.connect("local", 7777);
  }

  std::uint16_t unused_port() override { return 7777; }

private:
  std::shared_ptr<LocalHub> hub_ = std::make_shared<LocalHub>();
};

#ifdef ONLINE_E2E_WITH_GNS
class GnsDriver final : public Driver {
public:
  explicit GnsDriver(const Options &options) : options_(options) {}

  [[nodiscard]] std::string_view name() const override { return "gns"; }

  [[nodiscard]] BackendCaps caps() const override {
    BackendCaps caps;
    caps.max_clients = 4;
    return caps;
  }

  [[nodiscard]] std::uint16_t bound_port() const override { return port_; }

  std::unique_ptr<Transport> make_endpoint(FaultConfig faults = {}) override {
    return online_testing::maybe_wrap_faults(
        std::make_unique<GnsTransport>(), faults);
  }

  bool listen(Transport &transport) override {
    for (int attempt = 0; attempt < 100; ++attempt) {
      const std::uint16_t candidate = take_candidate();
      if (transport.listen(candidate)) {
        port_ = candidate;
        return true;
      }
    }
    return false;
  }

  bool connect(Transport &transport) override {
    return transport.connect(options_.host, port_);
  }

  std::uint16_t unused_port() override {
    for (int attempt = 0; attempt < 100; ++attempt) {
      const std::uint16_t candidate = take_candidate();
      GnsTransport probe;
      if (probe.listen(candidate)) {
        probe.close();
        return candidate;
      }
    }
    return 0;
  }

private:
  std::uint16_t take_candidate() {
    static std::uint16_t next = 0;
    if (next == 0)
      next = options_.port_base;
    const std::uint16_t candidate = next;
    next = next >= 65000 ? options_.port_base
                          : static_cast<std::uint16_t>(next + 1);
    return candidate;
  }

  const Options &options_;
  std::uint16_t port_ = 0;
};
#endif

inline std::string endpoint_name(std::size_t client_index) {
  return "client" + std::to_string(client_index);
}

inline int count_connected_on(const std::vector<RecordedEvent> &log,
                              std::string_view endpoint) {
  int count = 0;
  for (const auto &recorded : log) {
    if (recorded.endpoint == endpoint &&
        std::holds_alternative<Connected>(recorded.event))
      ++count;
  }
  return count;
}

inline std::vector<Packet> sequence_packets_on(
    const std::vector<RecordedEvent> &log, std::string_view endpoint) {
  std::vector<Packet> out;
  for (const auto &packet : online_testing::packets_on(log, endpoint)) {
    if (online_testing::decode_sequence_packet(packet.payload))
      out.push_back(packet);
  }
  return out;
}

struct Session {
  Driver &driver;
  const Options &options;
  std::unique_ptr<Transport> server;
  std::vector<std::unique_ptr<Transport>> clients;
  std::vector<PeerId> client_peers;
  EventPump pump;

  Session(Driver &driver_in, const Options &options_in,
          FaultConfig server_faults = {})
      : driver(driver_in), options(options_in),
        server(driver.make_endpoint(std::move(server_faults))),
        pump(std::vector<NamedEndpoint>{{"server", server.get()}}) {}

  void refresh_pump() {
    std::vector<NamedEndpoint> endpoints{{"server", server.get()}};
    for (std::size_t i = 0; i < clients.size(); ++i)
      endpoints.push_back(NamedEndpoint{endpoint_name(i), clients[i].get()});
    pump.set_endpoints(std::move(endpoints));
  }

  bool listen() { return driver.listen(*server); }
  [[nodiscard]] std::uint16_t port() const { return driver.bound_port(); }
  [[nodiscard]] const std::vector<RecordedEvent> &log() const {
    return pump.log();
  }
  void clear_log() { pump.clear_log(); }

  template <typename Predicate>
  bool pump_until(Predicate predicate, int timeout_ms = -1) {
    return pump.pump_until(std::move(predicate),
                           timeout_ms >= 0 ? timeout_ms : options.timeout_ms,
                           options.poll_ms);
  }

  void pump_for(int duration_ms) { pump.pump_for(duration_ms, options.poll_ms); }

  std::size_t add_client(FaultConfig faults = {}) {
    clients.push_back(driver.make_endpoint(std::move(faults)));
    client_peers.push_back(kInvalidPeer);
    refresh_pump();
    return clients.size() - 1;
  }

  Transport &client(std::size_t index) { return *clients.at(index); }
  [[nodiscard]] PeerId client_peer(std::size_t index) const {
    return client_peers.at(index);
  }

  bool learn_client_peer(std::size_t index) {
    Transport &endpoint = client(index);
    if (endpoint.local_peer_id() != kInvalidPeer) {
      client_peers[index] = endpoint.local_peer_id();
      return true;
    }
    std::unordered_set<PeerId> known(client_peers.begin(), client_peers.end());
    known.erase(kInvalidPeer);
    const bool found = pump_until([&](const std::vector<RecordedEvent> &events) {
      for (const auto &recorded : events) {
        if (recorded.endpoint != "server")
          continue;
        if (const auto *connected = std::get_if<Connected>(&recorded.event);
            connected && !known.contains(connected->peer))
          return true;
      }
      return false;
    });
    if (!found)
      return false;
    for (const auto &recorded : log()) {
      if (recorded.endpoint != "server")
        continue;
      if (const auto *connected = std::get_if<Connected>(&recorded.event);
          connected && !known.contains(connected->peer)) {
        client_peers[index] = connected->peer;
        endpoint.set_local_peer_id(connected->peer);
        return true;
      }
    }
    return false;
  }

  bool connect_client(std::size_t index, bool learn_peer = true) {
    if (!driver.connect(client(index)))
      return false;
    refresh_pump();
    const std::string name = endpoint_name(index);
    if (!pump_until([&](const std::vector<RecordedEvent> &events) {
          return online_testing::has_connected(events, name, kServerPeer);
        }))
      return false;
    return !learn_peer || learn_client_peer(index);
  }

  // For simultaneous joins, server Connected order is not a portable
  // client-index mapping. Each client announces its index; Packet.from is
  // the authoritative mapping.
  bool identify_clients_by_token() {
    for (std::size_t i = 0; i < clients.size(); ++i) {
      auto payload = bytes_from("identify-" + std::to_string(i));
      if (!client(i).send(kServerPeer, payload, Channel::Reliable))
        return false;
    }
    const bool found = pump_until([&](const std::vector<RecordedEvent> &events) {
      std::unordered_set<std::size_t> seen;
      for (const auto &packet : online_testing::packets_on(events, "server")) {
        const std::string_view text = view_bytes(packet.payload);
        if (text.starts_with("identify-"))
          seen.insert(static_cast<std::size_t>(std::stoi(std::string(text.substr(9)))));
      }
      return seen.size() == clients.size();
    });
    if (!found)
      return false;
    for (const auto &packet : online_testing::packets_on(log(), "server")) {
      const std::string_view text = view_bytes(packet.payload);
      if (!text.starts_with("identify-"))
        continue;
      const auto index =
          static_cast<std::size_t>(std::stoi(std::string(text.substr(9))));
      if (index < clients.size()) {
        client_peers[index] = packet.from;
        client(index).set_local_peer_id(packet.from);
      }
    }
    return true;
  }

  bool start(std::size_t client_count,
             const std::vector<FaultConfig> &client_faults = {},
             bool clear_setup_log = true) {
    if (!listen())
      return false;
    for (std::size_t i = 0; i < client_count; ++i) {
      FaultConfig faults = i < client_faults.size() ? client_faults[i] : FaultConfig{};
      const std::size_t index = add_client(std::move(faults));
      if (!connect_client(index))
        return false;
    }
    if (clear_setup_log)
      clear_log();
    return true;
  }

  bool reconnect_client(std::size_t index) {
    client_peers.at(index) = kInvalidPeer;
    return connect_client(index, true);
  }

  void close_clients() {
    for (auto &endpoint : clients)
      endpoint->close();
  }

  void close_all() {
    close_clients();
    server->close();
  }
};

inline bool send_text(Transport &from, PeerId to, std::string_view text,
                      Channel channel = Channel::Reliable) {
  return from.send(to, bytes_from(text), channel);
}

inline std::optional<Packet> copy_packet(const Packet *packet) {
  if (!packet)
    return std::nullopt;
  return *packet;
}

inline std::optional<Packet> wait_for_packet(Session &session,
                                             std::string_view endpoint,
                                             std::string_view payload,
                                             int timeout_ms = -1) {
  const bool found = session.pump_until(
      [&](const std::vector<RecordedEvent> &events) {
        return online_testing::find_packet(events, endpoint, payload) != nullptr;
      },
      timeout_ms);
  if (!found)
    return std::nullopt;
  return copy_packet(online_testing::find_packet(session.log(), endpoint, payload));
}

inline bool wait_for_connected(Session &session, std::string_view endpoint,
                               PeerId peer, int timeout_ms = -1) {
  return session.pump_until(
      [&](const std::vector<RecordedEvent> &events) {
        return online_testing::has_connected(events, endpoint, peer);
      },
      timeout_ms);
}

inline bool wait_for_disconnected(Session &session, std::string_view endpoint,
                                  PeerId peer, int timeout_ms = -1) {
  return session.pump_until(
      [&](const std::vector<RecordedEvent> &events) {
        return online_testing::has_disconnected(events, endpoint, peer);
      },
      timeout_ms);
}

inline std::vector<std::uint32_t> decoded_sequences_on(
    const std::vector<RecordedEvent> &log, std::string_view endpoint) {
  std::vector<std::uint32_t> out;
  for (const auto &packet : sequence_packets_on(log, endpoint)) {
    if (auto message = online_testing::decode_sequence_packet(packet.payload))
      out.push_back(message->sequence);
  }
  return out;
}

static void scenario_lifecycle_connect_and_ids(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.listen(), "server listens");
  ctx.expect(session.server->is_server(), "server role");
  ctx.expect(session.server->local_peer_id() == kServerPeer, "server id");

  const std::size_t count = static_cast<std::size_t>(std::max(ctx.options.clients, 1));
  std::unordered_set<PeerId> seen;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t index = session.add_client();
    ctx.require(session.connect_client(index), "client connects and learns id");
    const PeerId peer = session.client_peer(index);
    ctx.expect(peer >= kFirstRemotePeer, "remote peer id range");
    ctx.expect(!seen.contains(peer), "remote peer ids are unique");
    seen.insert(peer);
    ctx.expect(session.client(index).local_peer_id() == peer,
               "client local id matches server assignment");
    ctx.expect(!session.client(index).is_server(), "client role");
  }

  auto extra = driver.make_endpoint();
  ctx.require(driver.connect(*extra), "extra endpoint starts connect");
  if (driver.caps().client_id_on_connect) {
    ctx.expect(extra->local_peer_id() != kInvalidPeer,
               "backend assigns client id during connect");
  } else {
    ctx.expect(extra->local_peer_id() == kInvalidPeer,
               "backend leaves client id invalid until game handshake");
  }
  extra->close();
}

static void scenario_lifecycle_double_listen_and_reopen(TestContext &ctx,
                                                          Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.listen(), "first server listens");
  auto second = driver.make_endpoint();
  ctx.expect(!second->listen(session.port()), "second listen on same setup rejected");
  second->close();

  const std::uint16_t port = session.port();
  session.server->close();
  ctx.expect(!session.server->is_open(), "closed server reports closed");
  ctx.require(session.server->listen(port), "same endpoint can listen again after close");
  const std::size_t index = session.add_client();
  ctx.require(session.connect_client(index), "client connects after server reopen");
}

static void scenario_connect_without_server(TestContext &ctx, Driver &driver) {
  auto endpoint = driver.make_endpoint();
  const std::uint16_t port = driver.unused_port();
  const bool started = endpoint->connect(ctx.options.host, port);
  if (driver.caps().synchronous_connect_rejection) {
    ctx.expect(!started, "backend rejects connect without a server synchronously");
    endpoint->close();
    return;
  }

  ctx.expect(started, "socket backend starts asynchronous connect");
  EventPump pump(std::vector<NamedEndpoint>{{"client", endpoint.get()}});
  // GNS uses a 10-second initial connection timeout on this platform.
  // Wait for the terminal event instead of sampling an arbitrary short
  // window in which a still-connecting endpoint looks identical.
  const bool disconnected = pump.pump_until(
      [](const std::vector<RecordedEvent> &log) {
        return online_testing::has_disconnected(log, "client", kServerPeer);
      },
      12000, ctx.options.poll_ms);
  const bool connected =
      online_testing::has_connected(pump.log(), "client", kServerPeer);
  ctx.expect(!connected, "unreachable client does not report connected");
  ctx.expect(disconnected,
             "unreachable client receives a terminal Disconnected event");
  ctx.note("unreachable observation: connected=" + std::to_string(connected) +
           " disconnected=" + std::to_string(disconnected));
  endpoint->close();
}

static void scenario_routing_unicast_and_broadcast(TestContext &ctx,
                                                     Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(2), "two clients connect");

  ctx.require(send_text(session.client(0), kServerPeer, "up"), "client sends up");
  auto up = wait_for_packet(session, "server", "up");
  ctx.expect(up && up->from == session.client_peer(0), "server sees client0 as sender");

  session.clear_log();
  ctx.require(send_text(*session.server, session.client_peer(1), "only-client1"),
              "server unicasts");
  ctx.expect(wait_for_packet(session, "client1", "only-client1").has_value(),
             "client1 receives unicast");
  session.pump_for(75);
  ctx.expect(!online_testing::find_packet(session.log(), "client0", "only-client1"),
             "client0 does not receive unicast for client1");

  session.clear_log();
  ctx.require(session.server->broadcast(bytes_from("all"), Channel::Reliable),
              "server broadcasts");
  ctx.expect(wait_for_packet(session, "client0", "all").has_value(),
             "client0 receives server broadcast");
  ctx.expect(wait_for_packet(session, "client1", "all").has_value(),
             "client1 receives server broadcast");

  session.clear_log();
  ctx.require(session.client(0).broadcast(bytes_from("client-broadcast"),
                                          Channel::Reliable),
              "client broadcast call succeeds");
  ctx.expect(wait_for_packet(session, "server", "client-broadcast").has_value(),
             "server receives client broadcast");
  session.pump_for(75);
  const bool peer_received = online_testing::find_packet(
      session.log(), "client1", "client-broadcast") != nullptr;
  if (driver.caps().client_broadcast_to_clients)
    ctx.expect(peer_received, "local client broadcast reaches other clients");
  else
    ctx.expect(!peer_received, "socket client broadcast reaches server only");
}

static void scenario_routing_invalid_and_self_send(TestContext &ctx,
                                                     Driver &driver) {
  auto unopened = driver.make_endpoint();
  ctx.expect(!send_text(*unopened, kServerPeer, "closed"),
             "send before open fails");

  Session session(driver, ctx.options);
  ctx.require(session.start(2), "two clients connect");
  ctx.expect(!send_text(*session.server, 999999, "nobody"),
             "server send to unknown peer fails");
  ctx.expect(!send_text(*session.server, kServerPeer, "self"),
             "server send to itself fails");

  session.clear_log();
  const bool direct_sent =
      send_text(session.client(0), session.client_peer(1), "direct");
  ctx.expect(direct_sent, "client send call to another peer is defined");
  session.pump_for(100);
  const bool at_client1 =
      online_testing::find_packet(session.log(), "client1", "direct") != nullptr;
  const bool at_server =
      online_testing::find_packet(session.log(), "server", "direct") != nullptr;
  if (driver.caps().direct_client_to_client) {
    ctx.expect(at_client1 && !at_server, "local transport routes client-to-client");
  } else {
    ctx.expect(at_server && !at_client1,
               "socket client send is server-bound regardless of peer argument");
    ctx.note("backend-defined: socket client peer argument does not select a client");
  }

  session.client(1).close();
  ctx.expect(!send_text(session.client(1), kServerPeer, "after-close"),
             "send after close fails");
}

static void scenario_channels_and_reliable_order(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(1), "client connects");

  constexpr std::uint32_t kCount = 100;
  for (std::uint32_t i = 0; i < kCount; ++i) {
    auto payload = online_testing::encode_sequence_packet(i, "reliable");
    ctx.require(session.client(0).send(kServerPeer, payload, Channel::Reliable),
                "reliable sequence send succeeds");
  }
  const bool all = session.pump_until([&](const std::vector<RecordedEvent> &events) {
    return decoded_sequences_on(events, "server").size() >= kCount;
  });
  ctx.require(all, "all reliable sequence packets arrive");
  auto sequences = decoded_sequences_on(session.log(), "server");
  bool ordered = sequences.size() == kCount;
  for (std::uint32_t i = 0; ordered && i < kCount; ++i)
    ordered = sequences[i] == i;
  ctx.expect(ordered, "reliable sequence remains in order without duplicates");

  session.clear_log();
  ctx.require(send_text(session.client(0), kServerPeer, "chan-reliable",
                        Channel::Reliable),
              "reliable label send");
  ctx.require(send_text(session.client(0), kServerPeer, "chan-unreliable",
                        Channel::Unreliable),
              "unreliable label send");
  ctx.require(send_text(session.client(0), kServerPeer, "chan-nodelay",
                        Channel::UnreliableNoDelay),
              "no-delay label send");
  auto reliable = wait_for_packet(session, "server", "chan-reliable");
  auto unreliable = wait_for_packet(session, "server", "chan-unreliable");
  auto no_delay = wait_for_packet(session, "server", "chan-nodelay");
  ctx.expect(reliable && reliable->channel == Channel::Reliable,
             "reliable channel label survives");
  ctx.expect(unreliable && unreliable->channel == Channel::Unreliable,
             "unreliable channel label survives");
  if (driver.caps().exact_no_delay_label) {
    ctx.expect(no_delay && no_delay->channel == Channel::UnreliableNoDelay,
               "no-delay channel label survives locally");
  } else {
    ctx.expect(no_delay && no_delay->channel == Channel::Unreliable,
               "socket backend reports no-delay traffic as unreliable");
    ctx.note("backend-defined: received GNS flags only distinguish reliable");
  }

  session.clear_log();
  constexpr std::uint32_t kUnreliableCount = 50;
  for (std::uint32_t i = 0; i < kUnreliableCount; ++i) {
    auto payload = online_testing::encode_sequence_packet(i, "unreliable");
    (void)session.client(0).send(kServerPeer, payload, Channel::Unreliable);
  }
  session.pump_for(250);
  auto unreliable_sequences = decoded_sequences_on(session.log(), "server");
  SequenceTracker tracker;
  for (std::uint32_t sequence : unreliable_sequences)
    tracker.observe(sequence);
  ctx.expect(!unreliable_sequences.empty(), "some unreliable packets arrive");
  ctx.expect(tracker.duplicates() == 0, "unreliable packets are not duplicated");
  ctx.note("unreliable delivery=" + std::to_string(unreliable_sequences.size()) +
           "/" + std::to_string(kUnreliableCount) +
           " inversions=" + std::to_string(tracker.inversions()));
  if (driver.caps().direct_client_to_client) {
    ctx.expect(unreliable_sequences.size() == kUnreliableCount,
               "local unreliable delivery is complete");
    ctx.expect(tracker.inversions() == 0, "local unreliable delivery is ordered");
  }
}

static void scenario_payload_boundaries(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(1), "client connects");

  const std::vector<std::size_t> sizes{0, 1, 2, 63, 64, 255, 1200, 8192, 65536};
  for (std::size_t size : sizes) {
    if (size > static_cast<std::size_t>(std::max(ctx.options.payload_max, 0)))
      continue;
    std::vector<std::byte> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
      payload[i] = static_cast<std::byte>(
          static_cast<unsigned char>((i * 131u + size * 17u) % 251u));
    }
    session.clear_log();
    const bool sent =
        session.client(0).send(kServerPeer, payload, Channel::Reliable);
    ctx.expect(sent, "payload send size=" + std::to_string(size));
    if (!sent)
      continue;
    const bool found = session.pump_until([&](const std::vector<RecordedEvent> &events) {
      for (const auto &packet : online_testing::packets_on(events, "server")) {
        if (packet.payload == payload)
          return true;
      }
      return false;
    });
    ctx.expect(found, "payload arrives byte-for-byte size=" + std::to_string(size));
  }
}

static void scenario_disconnect_client_close(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(2), "two clients connect");
  const PeerId closing = session.client_peer(0);
  session.client(0).close();
  ctx.expect(wait_for_disconnected(session, "server", closing),
             "server observes client close");
  ctx.expect(!session.client(0).is_open(), "closing client reports closed");
  ctx.require(send_text(session.client(1), kServerPeer, "still-here"),
              "remaining client can still send");
  ctx.expect(wait_for_packet(session, "server", "still-here").has_value(),
             "server still receives from remaining client");
}

static void scenario_disconnect_server_drop(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(1), "client connects");
  const PeerId peer = session.client_peer(0);
  ctx.require(session.server->disconnect(peer), "server drops peer");
  ctx.expect(!session.server->disconnect(peer), "second server drop fails");
  ctx.expect(wait_for_disconnected(session, "server", peer),
             "server observes its own drop");
  ctx.expect(wait_for_disconnected(session, "client0", kServerPeer),
             "dropped client observes disconnect");
}

static void scenario_disconnect_server_close(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(2), "two clients connect");
  session.server->close();
  ctx.expect(wait_for_disconnected(session, "client0", kServerPeer),
             "client0 observes server close");
  ctx.expect(wait_for_disconnected(session, "client1", kServerPeer),
             "client1 observes server close");
  ctx.expect(!send_text(session.client(0), kServerPeer, "gone"),
             "client send after server close eventually fails");
}

static void scenario_capacity_and_extra_client(TestContext &ctx, Driver &driver) {
  const BackendCaps caps = driver.caps();
  const std::size_t accepted =
      caps.max_clients == std::numeric_limits<std::size_t>::max()
          ? 5
          : caps.max_clients;
  Session session(driver, ctx.options);
  ctx.require(session.start(accepted), "clients up to backend capacity connect");

  const std::size_t extra = session.add_client();
  const bool started = driver.connect(session.client(extra));
  session.refresh_pump();
  if (caps.max_clients == std::numeric_limits<std::size_t>::max()) {
    ctx.require(started, "extra local client starts");
    ctx.expect(wait_for_connected(session, endpoint_name(extra), kServerPeer),
               "extra local client connects (no local cap)");
    return;
  }

  ctx.expect(started, "extra socket client starts async connect");
  session.pump_for(750);
  ctx.expect(count_connected_on(session.log(), "server") == 0,
             "server does not accept a new client beyond capacity");
  ctx.expect(!online_testing::has_connected(session.log(), endpoint_name(extra),
                                            kServerPeer),
             "extra client is not welcomed at transport level");
  ctx.expect(online_testing::has_disconnected(session.log(), endpoint_name(extra),
                                              kServerPeer),
             "capacity-rejected client receives an explicit terminal event");
  ctx.require(send_text(session.client(0), kServerPeer, "capacity-survivor"),
              "accepted client still sends after rejection");
  ctx.expect(wait_for_packet(session, "server", "capacity-survivor").has_value(),
             "accepted client still works after rejection");
}

static void scenario_simultaneous_clients(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.listen(), "server listens");
  constexpr std::size_t kClients = 4;
  for (std::size_t i = 0; i < kClients; ++i)
    (void)session.add_client();
  for (std::size_t i = 0; i < kClients; ++i)
    ctx.require(driver.connect(session.client(i)), "simultaneous connect starts");
  session.refresh_pump();

  const bool all_connected = session.pump_until([&](const std::vector<RecordedEvent> &events) {
    if (count_connected_on(events, "server") != static_cast<int>(kClients))
      return false;
    for (std::size_t i = 0; i < kClients; ++i) {
      if (!online_testing::has_connected(events, endpoint_name(i), kServerPeer))
        return false;
    }
    return true;
  });
  ctx.require(all_connected, "all simultaneous clients connect");
  ctx.require(session.identify_clients_by_token(),
              "token handshake maps clients to peer ids");

  std::unordered_set<PeerId> peers;
  for (std::size_t i = 0; i < kClients; ++i) {
    peers.insert(session.client_peer(i));
    ctx.require(send_text(session.client(i), kServerPeer,
                          "simultaneous-" + std::to_string(i)),
                "simultaneous client sends");
  }
  ctx.expect(peers.size() == kClients, "simultaneous peer ids are unique");
  for (std::size_t i = 0; i < kClients; ++i) {
    auto packet = wait_for_packet(session, "server",
                                  "simultaneous-" + std::to_string(i));
    ctx.expect(packet && packet->from == session.client_peer(i),
               "server maps simultaneous packet to correct peer");
  }
}

static void scenario_flood_and_drain(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(1), "client connects");
  const auto count = static_cast<std::uint32_t>(std::max(ctx.options.flood_count, 1));
  for (std::uint32_t i = 0; i < count; ++i) {
    auto payload = online_testing::encode_sequence_packet(i, "flood");
    ctx.require(session.client(0).send(kServerPeer, payload, Channel::Reliable),
                "flood send succeeds");
  }
  const bool drained = session.pump_until(
      [&](const std::vector<RecordedEvent> &events) {
        return decoded_sequences_on(events, "server").size() >= count;
      },
      std::max(ctx.options.timeout_ms, 10000));
  ctx.require(drained, "flood eventually drains");
  auto sequences = decoded_sequences_on(session.log(), "server");
  SequenceTracker tracker;
  for (std::uint32_t sequence : sequences)
    tracker.observe(sequence);
  ctx.expect(tracker.unique_count() == count, "flood unique count");
  ctx.expect(tracker.duplicates() == 0, "flood has no duplicates");
  ctx.expect(tracker.inversions() == 0, "reliable flood remains ordered");
}

static void scenario_send_before_connected(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.listen(), "server listens");
  const std::size_t index = session.add_client();
  ctx.require(driver.connect(session.client(index)), "connect starts");
  session.refresh_pump();
  const bool sent = send_text(session.client(index), kServerPeer, "early");
  const bool connected =
      wait_for_connected(session, endpoint_name(index), kServerPeer);
  ctx.require(connected, "client eventually connects");
  session.pump_for(150);
  const bool arrived =
      online_testing::find_packet(session.log(), "server", "early") != nullptr;
  if (sent) {
    ctx.expect(arrived, "send-before-connected that reports success is delivered");
  } else {
    ctx.expect(!arrived, "send-before-connected that fails is not delivered");
  }
  ctx.note(std::string("send_before_connected: sent=") + (sent ? "true" : "false") +
           " arrived=" + (arrived ? "true" : "false"));
}

static void scenario_reconnect_after_drop(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(1), "client connects");
  const PeerId first_peer = session.client_peer(0);
  ctx.require(session.server->disconnect(first_peer), "server drops client");
  ctx.require(wait_for_disconnected(session, "client0", kServerPeer),
              "client observes drop before reconnect");
  session.client(0).close();
  session.clear_log();
  ctx.require(session.reconnect_client(0), "same endpoint reconnects after close");
  ctx.expect(session.client_peer(0) != first_peer,
             "reconnect receives a fresh peer id");
  ctx.require(send_text(session.client(0), kServerPeer, "back-again"),
              "reconnected client sends");
  auto packet = wait_for_packet(session, "server", "back-again");
  ctx.expect(packet && packet->from == session.client_peer(0),
             "server receives reconnected client under new id");
}

static void scenario_restart_server_same_port(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(1), "initial client connects");
  const std::uint16_t port = session.port();
  session.close_all();
  session.clear_log();
  ctx.require(session.server->listen(port), "server rebinds same port after close");
  ctx.require(session.reconnect_client(0), "client reconnects to restarted server");
  ctx.require(send_text(session.client(0), kServerPeer, "after-restart"),
              "client sends after restart");
  ctx.expect(wait_for_packet(session, "server", "after-restart").has_value(),
             "restarted server receives packet");
}

static FaultTransport *fault_endpoint(Transport &transport) {
  return dynamic_cast<FaultTransport *>(&transport);
}

static void scenario_fault_drop_first_and_retry(TestContext &ctx, Driver &driver) {
  FaultConfig client_faults;
  client_faults.seed = ctx.options.seed;
  client_faults.drop_first_sends = 1;
  Session session(driver, ctx.options);
  ctx.require(session.start(1, {client_faults}), "client connects through fault wrapper");

  ctx.require(send_text(session.client(0), kServerPeer, "retry-me"),
              "dropped send reports queued success");
  session.pump_for(100);
  ctx.expect(!online_testing::find_packet(session.log(), "server", "retry-me"),
             "first attempt is dropped");
  ctx.require(send_text(session.client(0), kServerPeer, "retry-me"),
              "retry send succeeds");
  ctx.expect(wait_for_packet(session, "server", "retry-me").has_value(),
             "retry is delivered");
  auto *fault = fault_endpoint(session.client(0));
  ctx.expect(fault && fault->stats().sends_dropped == 1, "fault stats count drop");
}

static void scenario_fault_duplicate_and_dedupe(TestContext &ctx, Driver &driver) {
  FaultConfig client_faults;
  client_faults.seed = ctx.options.seed;
  client_faults.duplicate_first_sends = 1;
  Session session(driver, ctx.options);
  ctx.require(session.start(1, {client_faults}), "client connects");

  auto payload = online_testing::encode_sequence_packet(42, "duplicate");
  ctx.require(session.client(0).send(kServerPeer, payload, Channel::Reliable),
              "duplicated send succeeds");
  const bool both = session.pump_until([&](const std::vector<RecordedEvent> &events) {
    return decoded_sequences_on(events, "server").size() >= 2;
  });
  ctx.require(both, "both duplicate copies arrive");
  SequenceTracker tracker;
  for (std::uint32_t sequence : decoded_sequences_on(session.log(), "server"))
    tracker.observe(sequence);
  ctx.expect(tracker.unique_count() == 1, "dedupe keeps one logical message");
  ctx.expect(tracker.duplicates() == 1, "dedupe observes one duplicate");
}

static void scenario_fault_delay(TestContext &ctx, Driver &driver) {
  FaultConfig server_faults;
  server_faults.seed = ctx.options.seed;
  server_faults.latency_ms = 60;
  Session session(driver, ctx.options, server_faults);
  ctx.require(session.start(1), "client connects");

  const auto started = std::chrono::steady_clock::now();
  ctx.require(send_text(session.client(0), kServerPeer, "slow"), "delayed send");
  ctx.require(wait_for_packet(session, "server", "slow").has_value(),
              "delayed packet eventually arrives");
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  ctx.expect(elapsed.count() >= 45, "packet was actually delayed");
  ctx.note("observed delay ms=" + std::to_string(elapsed.count()));
}

static void scenario_fault_reorder(TestContext &ctx, Driver &driver) {
  FaultConfig server_faults;
  server_faults.seed = ctx.options.seed;
  server_faults.latency_ms = 2;
  server_faults.reorder_every = 2;
  server_faults.reorder_extra_ms = 50;
  Session session(driver, ctx.options, server_faults);
  ctx.require(session.start(1), "client connects");

  for (std::uint32_t i = 0; i < 8; ++i) {
    auto payload = online_testing::encode_sequence_packet(i, "reorder");
    ctx.require(session.client(0).send(kServerPeer, payload, Channel::Reliable),
                "reorder sequence send");
  }
  const bool all = session.pump_until([&](const std::vector<RecordedEvent> &events) {
    return decoded_sequences_on(events, "server").size() >= 8;
  });
  ctx.require(all, "all reordered packets eventually arrive");
  SequenceTracker tracker;
  for (std::uint32_t sequence : decoded_sequences_on(session.log(), "server"))
    tracker.observe(sequence);
  ctx.expect(tracker.unique_count() == 8, "reorder keeps all messages");
  ctx.expect(tracker.inversions() > 0, "fault wrapper actually reorders");
  ctx.expect(tracker.missing_in_range() == 0, "resequencer sees a complete range");
}

static void scenario_fault_corrupt_then_valid(TestContext &ctx, Driver &driver) {
  FaultConfig client_faults;
  client_faults.seed = ctx.options.seed;
  client_faults.corruption = online_testing::Corruption::FlipByte;
  client_faults.corrupt_first_sends = 1;
  client_faults.corrupt_offset = 6;
  Session session(driver, ctx.options);
  ctx.require(session.start(1, {client_faults}), "client connects");

  auto first = online_testing::encode_sequence_packet(7, "checked");
  ctx.require(session.client(0).send(kServerPeer, first, Channel::Reliable),
              "corrupted send enters transport");
  const bool arrived = session.pump_until([&](const std::vector<RecordedEvent> &events) {
    return !online_testing::packets_on(events, "server").empty();
  });
  ctx.require(arrived, "corrupted bytes arrive as a packet");
  auto packets = online_testing::packets_on(session.log(), "server");
  ctx.require(!packets.empty(), "corrupted packet captured");
  ctx.expect(!online_testing::decode_sequence_packet(packets.front().payload),
             "checksum rejects corrupted packet");

  auto second = online_testing::encode_sequence_packet(8, "checked");
  ctx.require(session.client(0).send(kServerPeer, second, Channel::Reliable),
              "valid send after corruption");
  const bool valid = session.pump_until([&](const std::vector<RecordedEvent> &events) {
    auto sequences = decoded_sequences_on(events, "server");
    return std::ranges::contains(sequences, 8u);
  });
  ctx.expect(valid, "valid packet after corruption is accepted");
}

static void scenario_fault_blackout_and_recover(TestContext &ctx, Driver &driver) {
  FaultConfig client_faults;
  client_faults.seed = ctx.options.seed;
  client_faults.latency_ms = 1; // ensures the wrapper exists for toggling.
  Session session(driver, ctx.options);
  ctx.require(session.start(1, {client_faults}), "client connects");
  auto *fault = fault_endpoint(session.client(0));
  ctx.require(fault != nullptr, "fault wrapper is present");

  fault->set_blackout_sends(true);
  ctx.require(send_text(session.client(0), kServerPeer, "during-blackout"),
              "blackout send reports queued");
  session.pump_for(100);
  ctx.expect(!online_testing::find_packet(session.log(), "server", "during-blackout"),
             "blackout drops application packet");

  fault->set_blackout_sends(false);
  ctx.require(send_text(session.client(0), kServerPeer, "after-blackout"),
              "post-blackout send");
  ctx.expect(wait_for_packet(session, "server", "after-blackout").has_value(),
             "traffic recovers after blackout");
}

static void scenario_malformed_then_valid(TestContext &ctx, Driver &driver) {
  Session session(driver, ctx.options);
  ctx.require(session.start(1), "client connects");

  const std::vector<std::byte> garbage{
      static_cast<std::byte>(0xFF), static_cast<std::byte>(0x00),
      static_cast<std::byte>(0x7F)};
  ctx.require(session.client(0).send(kServerPeer, garbage, Channel::Reliable),
              "garbage bytes can enter byte-pipe transport");
  auto truncated = online_testing::encode_sequence_packet(3, "cut");
  truncated.resize(5);
  ctx.require(session.client(0).send(kServerPeer, truncated, Channel::Reliable),
              "truncated bytes can enter byte-pipe transport");
  session.pump_for(100);
  auto packets = online_testing::packets_on(session.log(), "server");
  ctx.expect(packets.size() >= 2, "malformed packets are observable to receiver logic");
  for (const auto &packet : packets) {
    ctx.expect(!online_testing::decode_sequence_packet(packet.payload),
               "malformed packet is rejected by codec");
  }

  auto valid = online_testing::encode_sequence_packet(4, "valid");
  ctx.require(session.client(0).send(kServerPeer, valid, Channel::Reliable),
              "valid packet after malformed traffic");
  const bool arrived = session.pump_until([&](const std::vector<RecordedEvent> &events) {
    auto sequences = decoded_sequences_on(events, "server");
    return std::ranges::contains(sequences, 4u);
  });
  ctx.expect(arrived, "receiver keeps working after malformed traffic");
}

static void scenario_fault_profile_soak(TestContext &ctx, Driver &driver) {
  FaultConfig client_faults = ctx.options.profile_faults;
  client_faults.seed = ctx.options.seed;
  FaultConfig server_faults = ctx.options.profile_faults;
  server_faults.seed = ctx.options.seed + 17u;
  Session session(driver, ctx.options, server_faults);
  ctx.require(session.start(1, {client_faults}), "profile client connects");

  constexpr std::uint32_t kPerChannel = 100;
  for (std::uint32_t i = 0; i < kPerChannel; ++i) {
    auto reliable = online_testing::encode_sequence_packet(i, "profile-reliable");
    (void)session.client(0).send(kServerPeer, reliable, Channel::Reliable);
    auto unreliable = online_testing::encode_sequence_packet(1000 + i, "profile-unreliable");
    (void)session.client(0).send(kServerPeer, unreliable, Channel::UnreliableNoDelay);
  }
  session.pump_for(1000);

  SequenceTracker tracker;
  std::size_t decoded = 0;
  std::size_t rejected = 0;
  for (const auto &packet : online_testing::packets_on(session.log(), "server")) {
    if (auto message = online_testing::decode_sequence_packet(packet.payload)) {
      tracker.observe(message->sequence);
      ++decoded;
    } else {
      ++rejected;
    }
  }
  ctx.note("profile=" + ctx.options.fault_profile +
           " decoded=" + std::to_string(decoded) +
           " unique=" + std::to_string(tracker.unique_count()) +
           " duplicates=" + std::to_string(tracker.duplicates()) +
           " inversions=" + std::to_string(tracker.inversions()) +
           " rejected=" + std::to_string(rejected));
  if (ctx.options.profile_faults.is_clean()) {
    ctx.expect(tracker.unique_count() == kPerChannel * 2,
               "clean profile delivers every sequence");
    ctx.expect(rejected == 0, "clean profile rejects nothing");
  } else {
    ctx.expect(tracker.unique_count() > 0,
               "fault profile still delivers some traffic");
    ctx.expect(tracker.unique_count() <= kPerChannel * 2,
               "fault profile cannot invent logical messages");
  }
}

struct Scenario {
  const char *name;
  const char *description;
  void (*run)(TestContext &, Driver &);
};

inline const std::vector<Scenario> &all_scenarios() {
  static const std::vector<Scenario> scenarios{
      {"lifecycle_connect_and_ids", "connect, roles, and peer-id assignment",
       scenario_lifecycle_connect_and_ids},
      {"lifecycle_double_listen_and_reopen", "port/hub conflicts and endpoint reuse",
       scenario_lifecycle_double_listen_and_reopen},
      {"connect_without_server", "unreachable endpoint behavior",
       scenario_connect_without_server},
      {"routing_unicast_and_broadcast", "server/client routing semantics",
       scenario_routing_unicast_and_broadcast},
      {"routing_invalid_and_self_send", "invalid destinations and closed sends",
       scenario_routing_invalid_and_self_send},
      {"channels_and_reliable_order", "channel labels and reliable ordering",
       scenario_channels_and_reliable_order},
      {"payload_boundaries", "empty through large payloads", scenario_payload_boundaries},
      {"disconnect_client_close", "graceful client departure",
       scenario_disconnect_client_close},
      {"disconnect_server_drop", "server-initiated peer drop",
       scenario_disconnect_server_drop},
      {"disconnect_server_close", "server shutdown notification",
       scenario_disconnect_server_close},
      {"capacity_and_extra_client", "peer capacity and rejection behavior",
       scenario_capacity_and_extra_client},
      {"simultaneous_clients", "concurrent joins and peer mapping",
       scenario_simultaneous_clients},
      {"flood_and_drain", "burst traffic and receive draining",
       scenario_flood_and_drain},
      {"send_before_connected", "early-send semantics", scenario_send_before_connected},
      {"reconnect_after_drop", "same endpoint reconnects with a fresh id",
       scenario_reconnect_after_drop},
      {"restart_server_same_port", "server process/endpoint restart",
       scenario_restart_server_same_port},
      {"fault_drop_first_and_retry", "application retry after injected loss",
       scenario_fault_drop_first_and_retry},
      {"fault_duplicate_and_dedupe", "duplicate delivery and sequence dedupe",
       scenario_fault_duplicate_and_dedupe},
      {"fault_delay", "one-way application latency", scenario_fault_delay},
      {"fault_reorder", "reordering and sequence recovery", scenario_fault_reorder},
      {"fault_corrupt_then_valid", "checksum rejection and recovery",
       scenario_fault_corrupt_then_valid},
      {"fault_blackout_and_recover", "temporary traffic blackout",
       scenario_fault_blackout_and_recover},
      {"malformed_then_valid", "garbage/truncated packets then valid traffic",
       scenario_malformed_then_valid},
      {"fault_profile_soak", "configurable mixed-traffic fault profile",
       scenario_fault_profile_soak},
  };
  return scenarios;
}

inline FaultConfig fault_profile_preset(std::string_view name, std::uint32_t seed) {
  FaultConfig config;
  config.seed = seed;
  if (name == "clean" || name == "custom")
    return config;
  if (name == "laggy") {
    config.latency_ms = 35;
    config.jitter_ms = 15;
    return config;
  }
  if (name == "lossy") {
    config.drop_send_rate = 0.10;
    config.drop_receive_rate = 0.05;
    config.duplicate_send_rate = 0.03;
    config.latency_ms = 10;
    config.jitter_ms = 10;
    return config;
  }
  if (name == "chaos") {
    config.drop_send_rate = 0.25;
    config.drop_receive_rate = 0.10;
    config.duplicate_send_rate = 0.10;
    config.corruption = online_testing::Corruption::FlipByte;
    config.corrupt_send_rate = 0.03;
    config.latency_ms = 15;
    config.jitter_ms = 30;
    config.reorder_every = 3;
    config.reorder_extra_ms = 25;
    return config;
  }
  throw std::invalid_argument("unknown fault profile: " + std::string(name));
}

inline std::vector<std::string> split_commas(std::string_view value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < value.size()) {
    const std::size_t comma = value.find(',', start);
    out.emplace_back(value.substr(start, comma == std::string_view::npos
                                              ? value.size() - start
                                              : comma - start));
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  return out;
}

inline int parse_int(const std::string &value, std::string_view option) {
  std::size_t used = 0;
  const int parsed = std::stoi(value, &used);
  if (used != value.size())
    throw std::invalid_argument("invalid integer for --" + std::string(option));
  return parsed;
}

inline double parse_double(const std::string &value, std::string_view option) {
  std::size_t used = 0;
  const double parsed = std::stod(value, &used);
  if (used != value.size())
    throw std::invalid_argument("invalid number for --" + std::string(option));
  return parsed;
}

inline bool parse_bool(const std::string &value, std::string_view option) {
  if (value == "1" || value == "true" || value == "yes" || value == "on")
    return true;
  if (value == "0" || value == "false" || value == "no" || value == "off")
    return false;
  throw std::invalid_argument("invalid boolean for --" + std::string(option));
}

inline void print_usage() {
  std::cout <<
      "afterhours-online E2E contract runner\n"
      "\n"
      "Selection:\n"
      "  --backend=local|gns|both       Backends to run (default depends on build)\n"
      "  --scenario=name[,name]         Exact scenario(s); repeatable\n"
      "  --filter=text                  Substring scenario filter\n"
      "  --list                         List scenarios and exit\n"
      "  --repeat=N                     Run the selected matrix N times\n"
      "  --fail-fast                    Stop after the first failure\n"
      "  --strict-gaps                  Treat known gaps (XFAIL) as failures\n"
      "  --json                         Emit JSON lines plus a summary object\n"
      "  --verbose                      Print scenario notes\n"
      "\n"
      "Setup and timing:\n"
      "  --clients=N                    Default clients for generic scenarios\n"
      "  --host=127.0.0.1               Socket host/address\n"
      "  --port-base=28100              First GNS port to scan\n"
      "  --timeout-ms=5000              Per-wait timeout\n"
      "  --poll-ms=1                    Pump sleep between polls\n"
      "  --flood-count=250              Reliable flood size\n"
      "  --payload-max=65536            Largest boundary payload to test\n"
      "  --seed=41232                   Deterministic fault RNG seed\n"
      "\n"
      "Fault profile (used by fault_profile_soak; overrides are order-sensitive):\n"
      "  --fault-profile=clean|laggy|lossy|chaos|custom\n"
      "  --drop-first-sends=N --drop-send-rate=0..1 --drop-receive-rate=0..1\n"
      "  --duplicate-first-sends=N --duplicate-send-rate=0..1\n"
      "  --corrupt-first-sends=N --corrupt-send-rate=0..1\n"
      "  --corruption=none|flip|truncate --corrupt-offset=N --truncate-to=N\n"
      "  --latency-ms=N --jitter-ms=N --reorder-every=N --reorder-extra-ms=N\n"
      "  --max-queued-events=N --force-channel=reliable|unreliable|nodelay|none\n";
}

inline Options parse_options(int argc, char **argv) {
  Options options;
  options.profile_faults = fault_profile_preset("clean", options.seed);

  auto value_for = [&](int &index, std::string_view arg,
                       std::string_view inline_value) -> std::string {
    if (!inline_value.empty())
      return std::string(inline_value);
    if (index + 1 >= argc)
      throw std::invalid_argument("missing value for " + std::string(arg));
    return argv[++index];
  };

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    }
    if (!arg.starts_with("--"))
      throw std::invalid_argument("unexpected argument: " + arg);
    arg = arg.substr(2);
    std::string inline_value;
    if (const std::size_t equals = arg.find('='); equals != std::string::npos) {
      inline_value = arg.substr(equals + 1);
      arg = arg.substr(0, equals);
    }

    auto value = [&]() { return value_for(i, "--" + arg, inline_value); };
    if (arg == "list") options.list = inline_value.empty() ? true : parse_bool(inline_value, arg);
    else if (arg == "fail-fast") options.fail_fast = inline_value.empty() ? true : parse_bool(inline_value, arg);
    else if (arg == "strict-gaps") options.strict_gaps = inline_value.empty() ? true : parse_bool(inline_value, arg);
    else if (arg == "json") options.json = inline_value.empty() ? true : parse_bool(inline_value, arg);
    else if (arg == "verbose") options.verbose = inline_value.empty() ? true : parse_bool(inline_value, arg);
    else if (arg == "backend") {
      const std::string backend = value();
      if (backend == "local") options.backend = BackendChoice::Local;
      else if (backend == "gns") options.backend = BackendChoice::Gns;
      else if (backend == "both") options.backend = BackendChoice::Both;
      else throw std::invalid_argument("unknown backend: " + backend);
    } else if (arg == "scenario") {
      auto names = split_commas(value());
      options.scenario_names.insert(options.scenario_names.end(), names.begin(), names.end());
    } else if (arg == "filter") options.filter = value();
    else if (arg == "repeat") options.repeat = parse_int(value(), arg);
    else if (arg == "timeout-ms") options.timeout_ms = parse_int(value(), arg);
    else if (arg == "poll-ms") options.poll_ms = parse_int(value(), arg);
    else if (arg == "clients") options.clients = parse_int(value(), arg);
    else if (arg == "flood-count") options.flood_count = parse_int(value(), arg);
    else if (arg == "payload-max") options.payload_max = parse_int(value(), arg);
    else if (arg == "seed") {
      options.seed = static_cast<std::uint32_t>(parse_int(value(), arg));
      options.profile_faults.seed = options.seed;
    } else if (arg == "host") options.host = value();
    else if (arg == "port-base") {
      const int port = parse_int(value(), arg);
      if (port < 1 || port > 65000)
        throw std::invalid_argument("--port-base must be in 1..65000");
      options.port_base = static_cast<std::uint16_t>(port);
    } else if (arg == "fault-profile") {
      options.fault_profile = value();
      options.profile_faults = fault_profile_preset(options.fault_profile, options.seed);
    } else if (arg == "drop-first-sends") options.profile_faults.drop_first_sends = parse_int(value(), arg);
    else if (arg == "drop-send-rate") options.profile_faults.drop_send_rate = parse_double(value(), arg);
    else if (arg == "drop-receive-rate") options.profile_faults.drop_receive_rate = parse_double(value(), arg);
    else if (arg == "dropped-send-reports-success") options.profile_faults.dropped_send_reports_success = parse_bool(value(), arg);
    else if (arg == "duplicate-first-sends") options.profile_faults.duplicate_first_sends = parse_int(value(), arg);
    else if (arg == "duplicate-send-rate") options.profile_faults.duplicate_send_rate = parse_double(value(), arg);
    else if (arg == "corrupt-first-sends") options.profile_faults.corrupt_first_sends = parse_int(value(), arg);
    else if (arg == "corrupt-send-rate") options.profile_faults.corrupt_send_rate = parse_double(value(), arg);
    else if (arg == "corruption") {
      const std::string corruption = value();
      if (corruption == "none") options.profile_faults.corruption = online_testing::Corruption::None;
      else if (corruption == "flip") options.profile_faults.corruption = online_testing::Corruption::FlipByte;
      else if (corruption == "truncate") options.profile_faults.corruption = online_testing::Corruption::Truncate;
      else throw std::invalid_argument("unknown corruption mode: " + corruption);
    } else if (arg == "corrupt-offset") options.profile_faults.corrupt_offset = static_cast<std::size_t>(parse_int(value(), arg));
    else if (arg == "truncate-to") options.profile_faults.truncate_to_bytes = static_cast<std::size_t>(parse_int(value(), arg));
    else if (arg == "force-channel") {
      const std::string channel = value();
      if (channel == "none") options.profile_faults.force_channel.reset();
      else if (channel == "reliable") options.profile_faults.force_channel = Channel::Reliable;
      else if (channel == "unreliable") options.profile_faults.force_channel = Channel::Unreliable;
      else if (channel == "nodelay") options.profile_faults.force_channel = Channel::UnreliableNoDelay;
      else throw std::invalid_argument("unknown channel: " + channel);
    } else if (arg == "latency-ms") options.profile_faults.latency_ms = parse_int(value(), arg);
    else if (arg == "jitter-ms") options.profile_faults.jitter_ms = parse_int(value(), arg);
    else if (arg == "reorder-every") options.profile_faults.reorder_every = parse_int(value(), arg);
    else if (arg == "reorder-extra-ms") options.profile_faults.reorder_extra_ms = parse_int(value(), arg);
    else if (arg == "max-queued-events") options.profile_faults.max_queued_events = static_cast<std::size_t>(parse_int(value(), arg));
    else if (arg == "apply-to-lifecycle-events") options.profile_faults.apply_to_lifecycle_events = parse_bool(value(), arg);
    else throw std::invalid_argument("unknown option: --" + arg);
  }

  if (options.repeat < 1 || options.timeout_ms < 1 || options.poll_ms < 0 ||
      options.clients < 1 || options.flood_count < 1 || options.payload_max < 0)
    throw std::invalid_argument("numeric options are out of range");
  return options;
}

struct Result {
  std::string backend;
  std::string scenario;
  std::string status;
  long long duration_ms = 0;
  std::vector<std::string> failures;
  std::vector<std::string> gaps;
  std::vector<std::string> notes;
  std::string skip_reason;
};

inline std::unique_ptr<Driver> make_driver(std::string_view backend,
                                           const Options &options) {
  (void)options;
  if (backend == "local")
    return std::make_unique<LocalDriver>();
#ifdef ONLINE_E2E_WITH_GNS
  if (backend == "gns")
    return std::make_unique<GnsDriver>(options);
#endif
  throw std::invalid_argument("backend is not compiled into this test binary: " +
                              std::string(backend));
}

inline std::vector<std::string> selected_backends(const Options &options) {
  switch (options.backend) {
  case BackendChoice::Local:
    return {"local"};
  case BackendChoice::Gns:
    return {"gns"};
  case BackendChoice::Both:
    return {"local", "gns"};
  }
  return {"local"};
}

inline std::vector<const Scenario *> selected_scenarios(const Options &options) {
  std::vector<const Scenario *> selected;
  for (const auto &scenario : all_scenarios()) {
    const std::string name = scenario.name;
    if (!options.scenario_names.empty() &&
        !std::ranges::contains(options.scenario_names, name))
      continue;
    if (!options.filter.empty() && !name.contains(options.filter))
      continue;
    selected.push_back(&scenario);
  }

  for (const std::string &wanted : options.scenario_names) {
    const bool exists = std::any_of(
        all_scenarios().begin(), all_scenarios().end(),
        [&](const Scenario &scenario) { return wanted == scenario.name; });
    if (!exists)
      throw std::invalid_argument("unknown scenario: " + wanted);
  }
  return selected;
}

inline Result run_one(const Options &options, std::string_view backend,
                      const Scenario &scenario) {
  TestContext ctx{options, std::string(backend), scenario.name, {}, {}, {}};
  Result result;
  result.backend = std::string(backend);
  result.scenario = scenario.name;
  const auto started = std::chrono::steady_clock::now();
  try {
    auto driver = make_driver(backend, options);
    scenario.run(ctx, *driver);
  } catch (const SkipScenario &skip) {
    result.skip_reason = skip.reason;
  } catch (const AbortScenario &) {
    // ctx.failures already explains the abort.
  } catch (const std::exception &error) {
    ctx.failures.push_back(std::string("unhandled exception: ") + error.what());
  } catch (...) {
    ctx.failures.emplace_back("unhandled non-standard exception");
  }
  result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  result.failures = std::move(ctx.failures);
  result.gaps = std::move(ctx.gaps);
  result.notes = std::move(ctx.notes);
  if (!result.skip_reason.empty())
    result.status = "SKIP";
  else if (!result.failures.empty())
    result.status = "FAIL";
  else if (!result.gaps.empty())
    result.status = "XFAIL";
  else
    result.status = "PASS";
  return result;
}

inline std::string json_escape(std::string_view value) {
  std::string out;
  for (char c : value) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out += c; break;
    }
  }
  return out;
}

inline void print_json_strings(const std::vector<std::string> &values) {
  std::cout << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i)
      std::cout << ',';
    std::cout << '"' << json_escape(values[i]) << '"';
  }
  std::cout << ']';
}

inline void print_result(const Result &result, const Options &options) {
  if (options.json) {
    std::cout << "{\"backend\":\"" << json_escape(result.backend)
              << "\",\"scenario\":\"" << json_escape(result.scenario)
              << "\",\"status\":\"" << result.status
              << "\",\"duration_ms\":" << result.duration_ms << ",\"failures\":";
    print_json_strings(result.failures);
    std::cout << ",\"gaps\":";
    print_json_strings(result.gaps);
    std::cout << ",\"notes\":";
    print_json_strings(result.notes);
    std::cout << ",\"skip_reason\":\"" << json_escape(result.skip_reason)
              << "\"}\n";
    return;
  }

  std::cout << '[' << result.status << "] " << result.backend << '/'
            << result.scenario << " (" << result.duration_ms << "ms)";
  if (!result.skip_reason.empty())
    std::cout << " - " << result.skip_reason;
  std::cout << '\n';
  for (const auto &failure : result.failures)
    std::cout << "  FAIL: " << failure << '\n';
  for (const auto &gap : result.gaps)
    std::cout << "  GAP: " << gap << '\n';
  if (options.verbose) {
    for (const auto &note : result.notes)
      std::cout << "  NOTE: " << note << '\n';
  }
}

int main(int argc, char **argv) {
  Options options;
  try {
    options = parse_options(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n\n";
    print_usage();
    return 2;
  }

  if (options.list) {
    for (const auto &scenario : all_scenarios())
      std::cout << scenario.name << " - " << scenario.description << '\n';
    return 0;
  }

  std::vector<const Scenario *> scenarios;
  std::vector<std::string> backends;
  try {
    scenarios = selected_scenarios(options);
    backends = selected_backends(options);
    for (const std::string &backend : backends) {
      if (backend == "gns") {
#ifdef ONLINE_E2E_WITH_GNS
        if (!gns_backend_linked())
          throw std::invalid_argument("GNS backend is not linked");
#else
        throw std::invalid_argument(
            "GNS backend is not compiled into this test binary");
#endif
      }
    }
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }

  if (!options.json) {
    std::cout << "afterhours-online E2E: backends=" << backends.size()
              << " scenarios=" << scenarios.size()
              << " repeat=" << options.repeat
              << " seed=" << options.seed
              << " fault_profile=" << options.fault_profile << '\n';
  }

  int passed = 0, failed = 0, skipped = 0, known_gaps = 0;
  bool stop = false;
  for (int repetition = 0; repetition < options.repeat && !stop; ++repetition) {
    for (const std::string &backend : backends) {
      for (const Scenario *scenario : scenarios) {
        Result result = run_one(options, backend, *scenario);
        if (options.repeat > 1)
          result.scenario += "#" + std::to_string(repetition + 1);
        print_result(result, options);
        if (result.status == "PASS") ++passed;
        else if (result.status == "FAIL") ++failed;
        else if (result.status == "SKIP") ++skipped;
        else if (result.status == "XFAIL") ++known_gaps;

        const bool strict_gap_failure =
            options.strict_gaps && result.status == "XFAIL";
        if (options.fail_fast && (result.status == "FAIL" || strict_gap_failure)) {
          stop = true;
          break;
        }
      }
      if (stop)
        break;
    }
  }

  if (options.json) {
    std::cout << "{\"summary\":{\"pass\":" << passed << ",\"fail\":" << failed
              << ",\"skip\":" << skipped << ",\"xfail\":" << known_gaps
              << "}}\n";
  } else {
    std::cout << "SUMMARY pass=" << passed << " fail=" << failed
              << " skip=" << skipped << " xfail=" << known_gaps << '\n';
  }
  return failed == 0 && (!options.strict_gaps || known_gaps == 0) ? 0 : 1;
}
