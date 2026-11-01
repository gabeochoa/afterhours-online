// The one opt-in translation unit for the standalone GNS backend.
//
// A game that wants GNS adds this file to its build and links
// GameNetworkingSockets_s (see scripts/build-gns.sh). Games that never
// compile this file have no GNS include or link dependency at all.
//
// This TU alone defines STEAMNETWORKINGSOCKETS_STANDALONELIB and calls
// the explicit SteamNetworkingSockets_LibV12() accessor (inside
// gns_impl.h), so a future steam.cpp can reuse the same implementation
// with the SteamAPI accessor without macro collisions.

#define STEAMNETWORKINGSOCKETS_STANDALONELIB
#include "gns_impl.h"

namespace afterhours::online {

GnsTransport::GnsTransport() : impl_(std::make_unique<Impl>()) {}
GnsTransport::~GnsTransport() { impl_->close(); }

bool GnsTransport::listen(std::uint16_t port) { return impl_->listen(port); }
bool GnsTransport::connect(std::string_view host, std::uint16_t port) {
  return impl_->connect(host, port);
}
void GnsTransport::close() { impl_->close(); }
bool GnsTransport::disconnect(PeerId peer) { return impl_->disconnect(peer); }
bool GnsTransport::send(PeerId to, std::span<const std::byte> payload,
                        Channel channel) {
  return impl_->send(to, payload, channel);
}
bool GnsTransport::broadcast(std::span<const std::byte> payload,
                             Channel channel) {
  return impl_->broadcast(payload, channel);
}
std::optional<Event> GnsTransport::poll() { return impl_->poll(); }

PeerId GnsTransport::local_peer_id() const { return impl_->local_id; }
bool GnsTransport::is_server() const { return impl_->server; }
bool GnsTransport::is_open() const { return impl_->open; }
void GnsTransport::set_local_peer_id(PeerId peer) { impl_->local_id = peer; }

std::vector<PeerId> GnsTransport::peers() const {
  std::vector<PeerId> out;
  if (impl_->server) {
    for (const auto &[peer, conn] : impl_->peer_to_conn) {
      out.push_back(peer);
      (void)conn;
    }
  } else if (impl_->client_connected) {
    out.push_back(kServerPeer);
  }
  return out;
}

bool gns_backend_linked() { return true; }

} // namespace afterhours::online
