// Two-endpoint GNS loopback test: a GnsTransport server and client in
// one process talking over real UDP on 127.0.0.1. This is the plan's
// highest-risk validation: it proves the static library, linking, the
// connection handshake, channel mapping, and peer drop end to end.
//
// Build (see makefile `gns-test` target): links gns.cpp +
// libGameNetworkingSockets_s.a from vendor/gns.

#include "../src/online/gns.h"
#include "../src/online/online.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <variant>
#include <vector>

using namespace afterhours::online;

static void expect(bool condition, const char *message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  assert(condition);
}

// Poll both endpoints until `done` reports true from gathered events or
// the deadline passes. Returns whatever events arrived meanwhile.
template <typename DoneFn>
static std::vector<Event> pump(GnsTransport &a, GnsTransport &b, DoneFn done,
                               int timeout_ms = 8000) {
  std::vector<Event> collected;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    for (GnsTransport *t : {&a, &b}) {
      while (auto event = t->poll()) {
        collected.push_back(*event);
        if (done(collected))
          return collected;
      }
    }
    if (done(collected))
      return collected;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return collected;
}

static bool has_connected(const std::vector<Event> &events, PeerId peer) {
  for (const auto &event : events) {
    if (const auto *c = std::get_if<Connected>(&event); c && c->peer == peer)
      return true;
  }
  return false;
}

static const Packet *find_packet(const std::vector<Event> &events,
                                  std::string_view payload) {
  for (const auto &event : events) {
    if (const auto *p = std::get_if<Packet>(&event);
        p && view_bytes(p->payload) == payload)
      return p;
  }
  return nullptr;
}

static bool has_disconnected(const std::vector<Event> &events, PeerId peer) {
  for (const auto &event : events) {
    if (const auto *d = std::get_if<Disconnected>(&event);
        d && d->peer == peer)
      return true;
  }
  return false;
}

int main() {
  expect(gns_backend_linked(), "gns backend linked");

  GnsTransport server;
  std::uint16_t port = 0;
  for (std::uint16_t candidate = 27901; candidate < 27910; ++candidate) {
    if (server.listen(candidate)) {
      port = candidate;
      break;
    }
  }
  expect(port != 0, "server listens on loopback port");
  expect(server.is_server() && server.local_peer_id() == kServerPeer,
         "server role/id");

  GnsTransport client;
  expect(client.connect("127.0.0.1", port), "client starts connect");

  auto connected = pump(server, client, [](const std::vector<Event> &e) {
    return has_connected(e, kFirstRemotePeer) && has_connected(e, kServerPeer);
  });
  expect(has_connected(connected, kFirstRemotePeer),
         "server sees client connected with allocated id");
  expect(has_connected(connected, kServerPeer), "client sees server connected");
  expect(server.peers() == std::vector<PeerId>{kFirstRemotePeer},
         "server peer list");
  expect(client.peers() == std::vector<PeerId>{kServerPeer},
         "client peer list");

  client.set_local_peer_id(kFirstRemotePeer);
  expect(client.local_peer_id() == kFirstRemotePeer, "client learns its id");

  expect(client.send(kServerPeer, bytes_from("ping"), Channel::Reliable),
         "client sends reliable");
  auto pinged = pump(server, client, [](const std::vector<Event> &e) {
    return find_packet(e, "ping") != nullptr;
  });
  const Packet *ping = find_packet(pinged, "ping");
  expect(ping != nullptr, "server receives ping");
  expect(ping && ping->from == kFirstRemotePeer, "ping sender id");
  expect(ping && ping->channel == Channel::Reliable, "ping channel reliable");

  expect(server.broadcast(bytes_from("pong"), Channel::Unreliable),
         "server broadcasts unreliable");
  auto ponged = pump(server, client, [](const std::vector<Event> &e) {
    return find_packet(e, "pong") != nullptr;
  });
  const Packet *pong = find_packet(ponged, "pong");
  expect(pong != nullptr && pong->from == kServerPeer, "client receives pong");
  expect(pong && pong->channel == Channel::Unreliable, "pong channel");

  expect(server.send(kFirstRemotePeer, bytes_from("tick"),
                     Channel::UnreliableNoDelay),
         "server sends no-delay");
  auto ticked = pump(server, client, [](const std::vector<Event> &e) {
    return find_packet(e, "tick") != nullptr;
  });
  expect(find_packet(ticked, "tick") != nullptr, "client receives tick");

  expect(server.disconnect(kFirstRemotePeer), "server drops client");
  expect(!server.disconnect(kFirstRemotePeer), "second drop reports false");
  auto dropped = pump(server, client, [](const std::vector<Event> &e) {
    return has_disconnected(e, kServerPeer) &&
           has_disconnected(e, kFirstRemotePeer);
  });
  expect(has_disconnected(dropped, kFirstRemotePeer),
         "server observes its drop");
  expect(has_disconnected(dropped, kServerPeer),
         "client observes server drop");
  expect(server.peers().empty(), "server peer list empty after drop");

  client.close();
  server.close();
  std::cout << "afterhours-online GNS loopback tests passed\n";
  return 0;
}
