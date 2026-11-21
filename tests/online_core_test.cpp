#include "../src/online/online.h"
#include "../src/online/testing.h"

#include <cassert>
#include <iostream>
#include <memory>
#include <variant>

using namespace afterhours::online;

static void expect(bool condition, const char *message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  assert(condition);
}

static void test_codec() {
  Writer writer;
  writer.u8(7);
  writer.u16(0xBEEF);
  writer.u32(0x12345678u);
  writer.u64(0x1122334455667788ull);
  writer.i32(-42);
  writer.f32(12.5f);
  writer.boolean(true);
  writer.string("slime");

  Reader reader(writer.span());
  expect(reader.u8() == 7, "u8");
  expect(reader.u16() == 0xBEEF, "u16");
  expect(reader.u32() == 0x12345678u, "u32");
  expect(reader.u64() == 0x1122334455667788ull, "u64");
  expect(reader.i32() == -42, "i32");
  expect(reader.f32() == 12.5f, "f32");
  expect(reader.boolean() == true, "bool");
  expect(reader.string() == "slime", "string");
  expect(reader.exhausted(), "codec consumes full packet");

  auto truncated = writer.bytes();
  truncated.resize(4);
  Reader short_reader(truncated);
  expect(!short_reader.string(), "truncated packet rejected");
}

static std::optional<Packet> next_packet(Transport &transport) {
  while (auto event = transport.poll()) {
    if (auto *packet = std::get_if<Packet>(&*event))
      return *packet;
  }
  return std::nullopt;
}

static void test_local_transport() {
  auto hub = std::make_shared<LocalHub>();
  LocalTransport no_server(hub);
  expect(!no_server.connect("local", 7777), "connect needs server");

  LocalTransport server(hub);
  expect(server.listen(7777), "server listens");
  LocalTransport second_server(hub);
  expect(!second_server.listen(7777), "second server rejected");

  LocalTransport a(hub), b(hub);
  expect(a.connect("local", 7777), "client a connects");
  expect(b.connect("local", 7777), "client b connects");
  expect(server.is_server() && !a.is_server(), "roles");
  expect(a.local_peer_id() == kFirstRemotePeer, "first remote id");
  expect(b.local_peer_id() == kFirstRemotePeer + 1, "second remote id");

  auto hello = bytes_from("hello");
  expect(a.send(kServerPeer, hello, Channel::Reliable), "a sends to server");
  auto got = next_packet(server);
  expect(got && got->from == a.local_peer_id(), "server receives from a");
  expect(got && view_bytes(got->payload) == "hello", "payload survives");
  expect(got && got->channel == Channel::Reliable, "channel survives");

  auto all = bytes_from("all");
  expect(server.broadcast(all, Channel::Unreliable), "server broadcasts");
  auto got_a = next_packet(a);
  auto got_b = next_packet(b);
  expect(got_a && view_bytes(got_a->payload) == "all", "a gets broadcast");
  expect(got_b && view_bytes(got_b->payload) == "all", "b gets broadcast");

  const PeerId a_id = a.local_peer_id();
  a.close();
  bool saw_disconnect = false;
  while (auto event = server.poll()) {
    if (auto *disconnected = std::get_if<Disconnected>(&*event))
      saw_disconnect = disconnected->peer == a_id;
  }
  expect(saw_disconnect, "server sees disconnect");
}

static void test_local_disconnect_peer() {
  auto hub = std::make_shared<LocalHub>();
  LocalTransport server(hub);
  expect(server.listen(7777), "server listens (disconnect test)");
  LocalTransport client(hub);
  expect(client.connect("local", 7777), "client connects (disconnect test)");
  const PeerId client_id = client.local_peer_id();
  while (server.poll()) {
  }
  while (client.poll()) {
  }

  const auto server_stats = server.connection_stats(client_id);
  expect(server_stats && server_stats->ping_ms == 0 &&
             server_stats->quality_local == 1.f,
         "local stats report a perfect in-process link");
  expect(!server.connection_stats(424242), "unknown peer has no stats");

  expect(server.disconnect(client_id), "server drops one peer");
  expect(!server.disconnect(client_id), "second drop reports false");

  bool server_saw = false;
  while (auto event = server.poll()) {
    if (auto *d = std::get_if<Disconnected>(&*event))
      server_saw = d->peer == client_id &&
                   d->reason == DisconnectReason::LocalClose;
  }
  expect(server_saw, "server sees its own peer drop as a local close");

  bool client_saw = false;
  while (auto event = client.poll()) {
    if (auto *d = std::get_if<Disconnected>(&*event))
      client_saw = d->peer == kServerPeer &&
                   d->reason == DisconnectReason::ClosedByPeer;
  }
  expect(client_saw, "dropped client sees a closed-by-peer disconnect");
}

static void test_testing_sequence_envelope() {
  using namespace afterhours::online::testing;
  auto encoded = encode_sequence_packet(9, "body");
  auto decoded = decode_sequence_packet(encoded);
  expect(decoded && decoded->sequence == 9 && decoded->body == "body",
         "sequence envelope round-trips");

  encoded[6] = static_cast<std::byte>(
      std::to_integer<unsigned char>(encoded[6]) ^ 0xFFu);
  expect(!decode_sequence_packet(encoded),
         "sequence envelope checksum rejects corruption");

  SequenceTracker tracker;
  expect(tracker.observe(2) == SequenceTracker::Observation::Accepted,
         "tracker accepts first sequence");
  expect(tracker.observe(1) == SequenceTracker::Observation::Accepted,
         "tracker accepts reordered sequence");
  expect(tracker.observe(2) == SequenceTracker::Observation::Duplicate,
         "tracker detects duplicate");
  expect(tracker.unique_count() == 2 && tracker.duplicates() == 1 &&
             tracker.inversions() == 1 && tracker.missing_in_range() == 0,
         "tracker summarizes duplicate/reorder");
}

static void test_testing_fault_transport() {
  using namespace afterhours::online::testing;
  auto hub = std::make_shared<LocalHub>();
  LocalTransport server(hub);
  expect(server.listen(7777), "fault-test server listens");

  FaultConfig faults;
  faults.seed = 7;
  faults.drop_first_sends = 1;
  FaultTransport client(std::make_unique<LocalTransport>(hub), faults);
  expect(client.connect("local", 7777), "fault-test client connects");
  while (server.poll()) {
  }
  while (client.poll()) {
  }

  expect(client.send(kServerPeer, bytes_from("retry"), Channel::Reliable),
         "dropped send reports success by default");
  expect(!next_packet(server), "first send is dropped");
  expect(client.send(kServerPeer, bytes_from("retry"), Channel::Reliable),
         "retry send succeeds");
  auto retried = next_packet(server);
  expect(retried && view_bytes(retried->payload) == "retry", "retry arrives");
  expect(client.stats().sends_dropped == 1, "fault stats record drop");

  client.config().drop_first_sends = 0;
  client.config().duplicate_send_rate = 1.0;
  auto duplicate_payload = encode_sequence_packet(5, "twice");
  expect(client.send(kServerPeer, duplicate_payload, Channel::Reliable),
         "duplicated send succeeds");
  auto first = next_packet(server);
  auto second = next_packet(server);
  expect(first && second && first->payload == duplicate_payload &&
             second->payload == duplicate_payload,
         "duplicate copies both arrive");

  client.config().duplicate_send_rate = 0.0;
  client.config().corruption = Corruption::FlipByte;
  client.config().corrupt_send_rate = 1.0;
  client.config().corrupt_offset = 6;
  auto checked = encode_sequence_packet(6, "checked");
  expect(client.send(kServerPeer, checked, Channel::Reliable),
         "corrupted send succeeds at transport");
  auto corrupted = next_packet(server);
  expect(corrupted && !decode_sequence_packet(corrupted->payload),
         "corrupted sequence packet is rejected");
}

// ---- gap/online-session-layer: snapshot, codec ----

namespace session_layer_test {
struct Snap {
  float x = 0.f;
  float y = 0.f;
};
static Snap lerp_snap(const Snap &a, const Snap &b, float t) {
  return {a.x + (b.x - a.x) * t, 42.f}; // y proves "rest from newer"
}
enum class Msg : std::uint8_t { Hello = 3, Input = 1 };
struct Hello {
  std::uint32_t version = 0;
  std::uint8_t character = 0;
};
constexpr auto serialize(auto &archive, Hello &packet) {
  return archive(packet.version, packet.character);
}
} // namespace session_layer_test

static void test_snapshot_buffer_and_rate_gate() {
  using namespace session_layer_test;
  SnapshotBuffer<Snap> buffer(4);
  expect(!buffer.sample_delayed(6, lerp_snap), "empty buffer samples nothing");
  expect(buffer.push(10, {0.f, 1.f}), "first snapshot accepted");
  expect(buffer.push(12, {20.f, 2.f}), "newer snapshot accepted");
  expect(!buffer.push(12, {99.f, 9.f}), "duplicate tick rejected");
  expect(!buffer.push(11, {99.f, 9.f}), "stale tick rejected");
  expect(buffer.duplicates() == 1 && buffer.stale_dropped() == 1 &&
             buffer.accepted() == 2,
         "snapshot push accounting");
  // delay 1 behind newest(12) -> target 11, halfway 10..12
  auto mid = buffer.sample_delayed(1, lerp_snap);
  expect(mid && mid->x == 10.f && mid->y == 42.f,
         "interpolation lerps x, takes rest from newer");
  expect(buffer.push(13, {30.f, 3.f}) && buffer.push(14, {40.f, 4.f}) &&
             buffer.push(15, {50.f, 5.f}),
         "fill to capacity");
  expect(buffer.size() == 4 && buffer.newest_tick() == 15,
         "capacity bounds buffer, oldest pruned");
  auto past = buffer.sample(999, lerp_snap);
  expect(past && past->x == 50.f, "no extrapolation past newest");
  buffer.clear_keeping_newest();
  expect(buffer.size() == 1 && buffer.newest_tick() == 15,
         "clear_keeping_newest restarts around freshest");

  using namespace std::chrono_literals;
  RateGate gate = gate_for_hz(20.0); // 50ms
  const auto t0 = RateGate::Clock::time_point{} + 1s;
  expect(gate.try_take(t0), "rate gate first send allowed");
  expect(!gate.try_take(t0 + 49ms), "rate gate blocks inside period");
  expect(gate.try_take(t0 + 50ms), "rate gate opens at period");
  gate.reset();
  expect(gate.try_take(t0), "rate gate reset allows immediate send");
}

static void test_typed_codec() {
  using namespace session_layer_test;
  Hello hello{7u, 2u};
  const auto bytes = encode_typed(Msg::Hello, hello);
  expect(peek_typed<Msg>(bytes) == Msg::Hello, "typed codec peeks type");
  const auto back = decode_typed<Msg, Hello>(bytes, Msg::Hello);
  expect(back && back->version == 7u && back->character == 2u,
         "typed codec round-trips");
  expect(!decode_typed<Msg, Hello>(bytes, Msg::Input),
         "typed codec rejects wrong expected type");
  auto trailing = bytes;
  trailing.push_back(std::byte{0});
  expect(!decode_typed<Msg, Hello>(trailing, Msg::Hello),
         "typed codec requires exhaustion");
  auto truncated = bytes;
  truncated.pop_back();
  expect(!decode_typed<Msg, Hello>(truncated, Msg::Hello),
         "typed codec rejects truncated input");
  expect(!peek_typed<Msg>(std::span<const std::byte>{}),
         "typed codec peek rejects empty input");
}

int main() {
  test_codec();
  test_snapshot_buffer_and_rate_gate();
  test_typed_codec();
  test_local_transport();
  test_local_disconnect_peer();
  test_testing_sequence_envelope();
  test_testing_fault_transport();
  expect(protocol_hash("slime-v1") == protocol_hash("slime-v1"),
         "protocol hash stable");
  expect(protocol_hash("slime-v1") != protocol_hash("slime-v2"),
         "protocol hash distinguishes labels");
  std::cout << "afterhours-online core tests passed\n";
  return 0;
}
