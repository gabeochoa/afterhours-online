#include "../src/online/online.h"

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

  expect(server.disconnect(client_id), "server drops one peer");
  expect(!server.disconnect(client_id), "second drop reports false");

  bool server_saw = false;
  while (auto event = server.poll()) {
    if (auto *d = std::get_if<Disconnected>(&*event))
      server_saw = d->peer == client_id;
  }
  expect(server_saw, "server sees its own peer drop");

  bool client_saw = false;
  while (auto event = client.poll()) {
    if (auto *d = std::get_if<Disconnected>(&*event))
      client_saw = d->peer == kServerPeer;
  }
  expect(client_saw, "dropped client sees disconnect");
}

int main() {
  test_codec();
  test_local_transport();
  test_local_disconnect_peer();
  expect(protocol_hash("slime-v1") == protocol_hash("slime-v1"),
         "protocol hash stable");
  expect(protocol_hash("slime-v1") != protocol_hash("slime-v2"),
         "protocol hash distinguishes labels");
  std::cout << "afterhours-online core tests passed\n";
  return 0;
}
