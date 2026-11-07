#pragma once

// Standalone GameNetworkingSockets backend (direct IP / LAN).
//
// Opt-in, like every non-header backend: a game that wants GNS adds
// `src/online/gns.cpp` to its build and links the static library from
// `scripts/build-gns.sh` (`-lGameNetworkingSockets_s` plus its
// protobuf/OpenSSL deps). This header is safe to include anywhere: it
// pulls in no GNS/Steam headers and no link dependency by itself.
//
// Semantics match LocalTransport where the wire allows it:
// - listen() puts the endpoint in server mode (local id kServerPeer);
//   accepted clients get server-allocated ids from kFirstRemotePeer.
// - connect() makes a client whose local id stays kInvalidPeer until
//   the game protocol learns its assigned id and calls
//   set_local_peer_id() (see the Hello/Welcome handshake in the game).
// - Channel maps 1:1 to the GNS send flags; nothing is silently
//   upgraded to reliable.

#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "core.h"

namespace afterhours::online {

class GnsTransport final : public Transport {
public:
  GnsTransport();
  ~GnsTransport() override;

  bool listen(std::uint16_t port) override;
  bool connect(std::string_view host, std::uint16_t port) override;
  void close() override;
  bool disconnect(PeerId peer) override;
  bool send(PeerId to, std::span<const std::byte> payload,
            Channel channel) override;
  bool broadcast(std::span<const std::byte> payload, Channel channel) override;
  std::optional<Event> poll() override;

  [[nodiscard]] PeerId local_peer_id() const override;
  [[nodiscard]] bool is_server() const override;
  [[nodiscard]] bool is_open() const override;
  [[nodiscard]] std::optional<ConnectionStats>
  connection_stats(PeerId peer) const override;

  // Client only: record the server-assigned id from the game handshake.
  void set_local_peer_id(PeerId peer) override;

  // Connected peers (server: accepted clients; client: {kServerPeer}
  // once connected). Useful for session bookkeeping and tests.
  [[nodiscard]] std::vector<PeerId> peers() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// True when this binary was linked with gns.cpp. The weak fallback in
// core consumers is simply not calling GnsTransport; this helper lets a
// game compiled with the backend report it at runtime (menus, --help).
[[nodiscard]] bool gns_backend_linked();

} // namespace afterhours::online
