#pragma once

// Header-only, transport-agnostic multiplayer primitives for afterhours
// games. This file intentionally has no ECS, socket, Steam, or GNS
// dependency: a game can include it without changing how it links.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace afterhours::online {

inline constexpr std::uint32_t kProtocolVersion = 1;

using PeerId = std::int32_t;
inline constexpr PeerId kInvalidPeer = -1;
inline constexpr PeerId kServerPeer = 0;
inline constexpr PeerId kFirstRemotePeer = 10000;

enum class Channel : std::uint8_t {
  Reliable,
  Unreliable,
  UnreliableNoDelay,
};

struct Connected {
  PeerId peer;
};

struct Disconnected {
  PeerId peer;
};

struct Packet {
  PeerId from = kInvalidPeer;
  std::vector<std::byte> payload;
  Channel channel = Channel::Unreliable;
};

using Event = std::variant<Connected, Disconnected, Packet>;

class Transport {
public:
  Transport() = default;
  Transport(const Transport &) = delete;
  Transport &operator=(const Transport &) = delete;
  virtual ~Transport() = default;

  // Put this endpoint in authoritative-host mode. Local transports ignore
  // the port; socket transports bind to it.
  virtual bool listen(std::uint16_t port) = 0;

  // Connect as a client. Local transports ignore host/port and attach to
  // their in-process hub; socket transports interpret them normally.
  virtual bool connect(std::string_view host, std::uint16_t port) = 0;

  // Close this endpoint and notify connected peers where the backend can.
  virtual void close() = 0;

  // Drop one peer (server rejecting a join, client leaving). Backends that
  // cannot drop individual peers return false. The dropped peer observes a
  // Disconnected event on its own endpoint where the backend can deliver it.
  virtual bool disconnect(PeerId) { return false; }

  virtual bool send(PeerId to, std::span<const std::byte> payload,
                    Channel channel) = 0;
  virtual bool broadcast(std::span<const std::byte> payload,
                         Channel channel) = 0;
  virtual std::optional<Event> poll() = 0;

  // Client endpoints learn their server-assigned id from the game
  // handshake (the transport cannot know it at connect time). Backends
  // that assign ids locally ignore this.
  virtual void set_local_peer_id(PeerId) {}

  [[nodiscard]] virtual PeerId local_peer_id() const = 0;
  [[nodiscard]] virtual bool is_server() const = 0;
  [[nodiscard]] virtual bool is_open() const = 0;
};

inline std::vector<std::byte> bytes_from(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (char c : text)
    out.push_back(static_cast<std::byte>(c));
  return out;
}

inline std::string_view view_bytes(std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

// Small stable hash for a protocol label. It is a compatibility guard, not
// a security mechanism.
inline consteval std::uint32_t protocol_hash(std::string_view label) {
  std::uint32_t hash = 2166136261u;
  for (char c : label) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  return hash;
}

} // namespace afterhours::online
