#pragma once

// GNS transport implementation. Included ONLY by gns.cpp (one TU per
// game that opts into GNS), never by game code.
//
// Start with docs/gns-internals.md for the guided tour. In short:
// detail:: owns the process-wide GNS lifetime and the two registries
// that route GNS's one global status callback back to the Impl that
// owns a handle. Impl is one endpoint (server XOR client); it turns
// GNS listen/poll-group/message APIs into portable Events queued in
// `pending`, and turns portable PeerIds back into native connection
// handles on send. Read listen/connect, then poll/drain_messages,
// then on_status_changed, then close/disconnect last (ordering bugs
// live in what gets unregistered before a native close).
//
// Known seam (docs/gaps.md item 8): acquisition below currently calls
// the standalone LibV12 accessor itself. The connection logic after
// acquisition is the part a future steam.cpp should share once each
// backend TU owns its own init/accessor choice.
//
// Model follows Pharmasea's internal GNS server/client (listen socket +
// poll group on the server, single connection on the client, status
// callback routing), with its known quirks fixed: channels are honored
// on every send path, peer ids are allocated here on the server, and
// connection events become transport Events instead of game callbacks.

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>

#include "gns.h"

namespace afterhours::online::detail {

// One process-wide GNS lifetime, refcounted across transports so a
// loopback test can run a server and a client in the same process.
inline int &gns_refcount() {
  static int count = 0;
  return count;
}
inline ISteamNetworkingSockets *&gns_iface() {
  static ISteamNetworkingSockets *iface = nullptr;
  return iface;
}

inline bool gns_acquire() {
  if (gns_refcount()++ > 0)
    return gns_iface() != nullptr;
  SteamDatagramErrMsg err;
  if (!GameNetworkingSockets_Init(nullptr, err)) {
    gns_refcount() = 0;
    return false;
  }
  gns_iface() = SteamNetworkingSockets_LibV12();
  if (gns_iface() == nullptr) {
    GameNetworkingSockets_Kill();
    gns_refcount() = 0;
    return false;
  }
  if (auto *utils = SteamNetworkingUtils_LibV4()) {
    utils->SetDebugOutputFunction(
        k_ESteamNetworkingSocketsDebugOutputType_Warning,
        [](ESteamNetworkingSocketsDebugOutputType, const char *msg) {
          std::fprintf(stderr, "[gns] %s\n", msg);
        });
  }
  return true;
}

inline void gns_release() {
  if (--gns_refcount() <= 0) {
    gns_refcount() = 0;
    gns_iface() = nullptr;
    GameNetworkingSockets_Kill();
  }
}

inline int gns_send_flags(Channel channel) {
  switch (channel) {
  case Channel::Reliable:
    return k_nSteamNetworkingSend_Reliable;
  case Channel::Unreliable:
    return k_nSteamNetworkingSend_Unreliable;
  case Channel::UnreliableNoDelay:
    return k_nSteamNetworkingSend_UnreliableNoDelay;
  }
  return k_nSteamNetworkingSend_Unreliable;
}

// App-range (1000-1999) end reason this backend sends when it rejects
// a connection for capacity, so the client can tell "server full"
// apart from an ordinary application close.
inline constexpr int kEndReasonCapacity = 1001;

inline DisconnectReason map_end_reason(int code, bool problem_locally) {
  if (code == kEndReasonCapacity)
    return DisconnectReason::Rejected;
  if (code == k_ESteamNetConnectionEnd_Remote_Timeout ||
      code == k_ESteamNetConnectionEnd_Misc_Timeout)
    return DisconnectReason::Unreachable;
  if (code >= k_ESteamNetConnectionEnd_App_Min &&
      code <= k_ESteamNetConnectionEnd_AppException_Max)
    return DisconnectReason::ClosedByPeer;
  if (code >= k_ESteamNetConnectionEnd_Local_Min &&
      code <= k_ESteamNetConnectionEnd_Local_Max)
    return DisconnectReason::LocalProblem;
  if (code >= k_ESteamNetConnectionEnd_Remote_Min &&
      code <= k_ESteamNetConnectionEnd_Remote_Max)
    return DisconnectReason::ClosedByPeer;
  return problem_locally ? DisconnectReason::LocalProblem
                         : DisconnectReason::Unknown;
}

struct GnsImplBase {
  virtual ~GnsImplBase() = default;
  virtual void
  on_status_changed(SteamNetConnectionStatusChangedCallback_t *info) = 0;
};

// Intentionally leaked: a transport may be destroyed from another
// TU's static destructor at process exit, after function-local statics
// here would already be gone. The maps must outlive every Impl.
inline std::map<HSteamListenSocket, GnsImplBase *> &gns_listen_registry() {
  static auto *registry = new std::map<HSteamListenSocket, GnsImplBase *>();
  return *registry;
}
inline std::map<HSteamNetConnection, GnsImplBase *> &gns_conn_registry() {
  static auto *registry = new std::map<HSteamNetConnection, GnsImplBase *>();
  return *registry;
}

inline void
gns_status_callback(SteamNetConnectionStatusChangedCallback_t *info) {
  if (auto it = gns_conn_registry().find(info->m_hConn);
      it != gns_conn_registry().end()) {
    it->second->on_status_changed(info);
    return;
  }
  const HSteamListenSocket listen = info->m_info.m_hListenSocket;
  if (listen != k_HSteamListenSocket_Invalid) {
    if (auto it = gns_listen_registry().find(listen);
        it != gns_listen_registry().end())
      it->second->on_status_changed(info);
  }
}

} // namespace afterhours::online::detail

namespace afterhours::online {

struct GnsTransport::Impl final : detail::GnsImplBase {
  static constexpr std::size_t kMaxPeers = 4;

  ISteamNetworkingSockets *iface = nullptr;
  bool acquired = false;
  bool open = false;
  bool server = false;
  PeerId local_id = kInvalidPeer;

  HSteamListenSocket listen_sock = k_HSteamListenSocket_Invalid;
  HSteamNetPollGroup poll_group = k_HSteamNetPollGroup_Invalid;
  HSteamNetConnection client_conn = k_HSteamNetConnection_Invalid;
  bool client_connected = false;

  std::map<HSteamNetConnection, PeerId> conn_to_peer;
  std::map<PeerId, HSteamNetConnection> peer_to_conn;
  PeerId next_peer = kFirstRemotePeer;
  std::deque<Event> pending;

  SteamNetworkingConfigValue_t callback_opt() {
    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               (void *)detail::gns_status_callback);
    return opt;
  }

  bool listen(std::uint16_t port) {
    if (open || !detail::gns_acquire())
      return false;
    acquired = true;
    iface = detail::gns_iface();

    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;
    SteamNetworkingConfigValue_t opt = callback_opt();
    listen_sock = iface->CreateListenSocketIP(addr, 1, &opt);
    if (listen_sock == k_HSteamListenSocket_Invalid) {
      close();
      return false;
    }
    poll_group = iface->CreatePollGroup();
    if (poll_group == k_HSteamNetPollGroup_Invalid) {
      close();
      return false;
    }
    detail::gns_listen_registry()[listen_sock] = this;
    server = true;
    local_id = kServerPeer;
    open = true;
    return true;
  }

  bool connect(std::string_view host, std::uint16_t port) {
    if (open || !detail::gns_acquire())
      return false;
    acquired = true;
    iface = detail::gns_iface();

    std::string target(host);
    target += ':';
    target += std::to_string(port);
    SteamNetworkingIPAddr addr;
    if (!addr.ParseString(target.c_str())) {
      close();
      return false;
    }
    SteamNetworkingConfigValue_t opt = callback_opt();
    client_conn = iface->ConnectByIPAddress(addr, 1, &opt);
    if (client_conn == k_HSteamNetConnection_Invalid) {
      close();
      return false;
    }
    detail::gns_conn_registry()[client_conn] = this;
    server = false;
    local_id = kInvalidPeer;
    open = true;
    return true;
  }

  void close() {
    if (!open && !acquired)
      return;
    open = false;
    client_connected = false;
    if (iface != nullptr) {
      for (auto &[conn, peer] : conn_to_peer) {
        detail::gns_conn_registry().erase(conn);
        iface->CloseConnection(conn, 0, "transport closed", false);
        (void)peer;
      }
      if (client_conn != k_HSteamNetConnection_Invalid) {
        detail::gns_conn_registry().erase(client_conn);
        iface->CloseConnection(client_conn, 0, "transport closed", false);
        client_conn = k_HSteamNetConnection_Invalid;
      }
      if (listen_sock != k_HSteamListenSocket_Invalid) {
        detail::gns_listen_registry().erase(listen_sock);
        iface->CloseListenSocket(listen_sock);
        listen_sock = k_HSteamListenSocket_Invalid;
      }
      if (poll_group != k_HSteamNetPollGroup_Invalid) {
        iface->DestroyPollGroup(poll_group);
        poll_group = k_HSteamNetPollGroup_Invalid;
      }
    }
    conn_to_peer.clear();
    peer_to_conn.clear();
    pending.clear();
    local_id = kInvalidPeer;
    server = false;
    iface = nullptr;
    if (acquired) {
      acquired = false;
      detail::gns_release();
    }
  }

  bool disconnect(PeerId peer) {
    if (!open || iface == nullptr)
      return false;
    if (!server) {
      if (peer != kServerPeer ||
          client_conn == k_HSteamNetConnection_Invalid)
        return false;
      detail::gns_conn_registry().erase(client_conn);
      iface->CloseConnection(client_conn, 0, "left", true);
      client_conn = k_HSteamNetConnection_Invalid;
      client_connected = false;
      pending.emplace_back(
          Disconnected{kServerPeer, DisconnectReason::LocalClose});
      return true;
    }
    auto it = peer_to_conn.find(peer);
    if (it == peer_to_conn.end())
      return false;
    const HSteamNetConnection conn = it->second;
    peer_to_conn.erase(it);
    conn_to_peer.erase(conn);
    detail::gns_conn_registry().erase(conn);
    iface->CloseConnection(conn, 0, "disconnected by server", true);
    pending.emplace_back(Disconnected{peer, DisconnectReason::LocalClose});
    return true;
  }

  bool send(PeerId to, std::span<const std::byte> payload, Channel channel) {
    if (!open || iface == nullptr)
      return false;
    HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
    if (server) {
      auto it = peer_to_conn.find(to);
      if (it == peer_to_conn.end())
        return false;
      conn = it->second;
    } else {
      if (client_conn == k_HSteamNetConnection_Invalid)
        return false;
      conn = client_conn;
    }
    const EResult result = iface->SendMessageToConnection(
        conn, payload.data(), static_cast<std::uint32_t>(payload.size()),
        detail::gns_send_flags(channel), nullptr);
    return result == k_EResultOK;
  }

  bool broadcast(std::span<const std::byte> payload, Channel channel) {
    if (!server)
      return send(kServerPeer, payload, channel);
    bool sent = false;
    // Copy peer list first: send() does not mutate maps today, but keep
    // this robust if a future send path drops dead peers inline.
    std::vector<PeerId> targets;
    for (const auto &[peer, conn] : peer_to_conn) {
      targets.push_back(peer);
      (void)conn;
    }
    for (PeerId peer : targets)
      sent = send(peer, payload, channel) || sent;
    return sent;
  }

  std::optional<ConnectionStats> connection_stats(PeerId peer) const {
    if (!open || iface == nullptr)
      return std::nullopt;
    HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
    if (server) {
      const auto it = peer_to_conn.find(peer);
      if (it == peer_to_conn.end())
        return std::nullopt;
      conn = it->second;
    } else {
      if (peer != kServerPeer ||
          client_conn == k_HSteamNetConnection_Invalid)
        return std::nullopt;
      conn = client_conn;
    }
    SteamNetConnectionRealTimeStatus_t raw{};
    if (iface->GetConnectionRealTimeStatus(conn, &raw, 0, nullptr) !=
        k_EResultOK)
      return std::nullopt;
    ConnectionStats stats;
    stats.ping_ms = raw.m_nPing;
    stats.quality_local = raw.m_flConnectionQualityLocal;
    stats.quality_remote = raw.m_flConnectionQualityRemote;
    stats.out_packets_per_sec = raw.m_flOutPacketsPerSec;
    stats.out_bytes_per_sec = raw.m_flOutBytesPerSec;
    stats.in_packets_per_sec = raw.m_flInPacketsPerSec;
    stats.in_bytes_per_sec = raw.m_flInBytesPerSec;
    stats.send_rate_bytes_per_sec = raw.m_nSendRateBytesPerSecond;
    stats.pending_unreliable = raw.m_cbPendingUnreliable;
    stats.pending_reliable = raw.m_cbPendingReliable;
    stats.sent_unacked_reliable = raw.m_cbSentUnackedReliable;
    stats.queue_time_us = static_cast<std::int64_t>(raw.m_usecQueueTime);
    stats.jitter_us = raw.m_usecMaxJitter;
    return stats;
  }

  std::optional<Event> poll() {
    if (!open || iface == nullptr)
      return std::nullopt;
    iface->RunCallbacks();
    drain_messages();
    if (pending.empty())
      return std::nullopt;
    Event event = std::move(pending.front());
    pending.pop_front();
    return event;
  }

  void drain_messages() {
    std::array<SteamNetworkingMessage_t *, 16> msgs{};
    for (int batch = 0; batch < 8; ++batch) {
      int count = 0;
      if (server) {
        if (poll_group == k_HSteamNetPollGroup_Invalid)
          return;
        count = iface->ReceiveMessagesOnPollGroup(
            poll_group, msgs.data(), static_cast<int>(msgs.size()));
      } else {
        if (client_conn == k_HSteamNetConnection_Invalid)
          return;
        count = iface->ReceiveMessagesOnConnection(
            client_conn, msgs.data(), static_cast<int>(msgs.size()));
      }
      if (count <= 0)
        return;
      for (int i = 0; i < count; ++i) {
        SteamNetworkingMessage_t *msg = msgs[static_cast<std::size_t>(i)];
        Packet packet;
        packet.channel =
            (msg->m_nFlags & k_nSteamNetworkingSend_Reliable)
                ? Channel::Reliable
                : Channel::Unreliable;
        if (server) {
          auto it = conn_to_peer.find(msg->m_conn);
          if (it == conn_to_peer.end()) {
            msg->Release();
            continue;
          }
          packet.from = it->second;
        } else {
          packet.from = kServerPeer;
        }
        const auto *begin = static_cast<const std::byte *>(msg->m_pData);
        packet.payload.assign(begin, begin + msg->m_cbSize);
        msg->Release();
        pending.emplace_back(std::move(packet));
      }
    }
  }

  void
  on_status_changed(SteamNetConnectionStatusChangedCallback_t *info) override {
    if (iface == nullptr)
      return;
    switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting: {
      if (!server)
        break;
      if (conn_to_peer.size() >= kMaxPeers) {
        iface->CloseConnection(info->m_hConn, detail::kEndReasonCapacity,
                               "server full", false);
        break;
      }
      if (iface->AcceptConnection(info->m_hConn) != k_EResultOK ||
          !iface->SetConnectionPollGroup(info->m_hConn, poll_group)) {
        iface->CloseConnection(info->m_hConn, 0, nullptr, false);
        break;
      }
      const PeerId peer = next_peer++;
      conn_to_peer[info->m_hConn] = peer;
      peer_to_conn[peer] = info->m_hConn;
      detail::gns_conn_registry()[info->m_hConn] = this;
      break;
    }
    case k_ESteamNetworkingConnectionState_Connected: {
      if (server) {
        if (auto it = conn_to_peer.find(info->m_hConn);
            it != conn_to_peer.end())
          pending.emplace_back(Connected{it->second});
      } else {
        client_connected = true;
        pending.emplace_back(Connected{kServerPeer});
      }
      break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
      std::fprintf(stderr, "[gns] connection ended: reason %d: %s\n",
                   info->m_info.m_eEndReason, info->m_info.m_szEndDebug);
      detail::gns_conn_registry().erase(info->m_hConn);
      const DisconnectReason reason = detail::map_end_reason(
          info->m_info.m_eEndReason,
          info->m_info.m_eState ==
              k_ESteamNetworkingConnectionState_ProblemDetectedLocally);
      if (server) {
        if (auto it = conn_to_peer.find(info->m_hConn);
            it != conn_to_peer.end()) {
          pending.emplace_back(Disconnected{it->second, reason});
          peer_to_conn.erase(it->second);
          conn_to_peer.erase(it);
        }
      } else if (info->m_hConn == client_conn) {
        // A connection attempt can end before Connected is delivered.
        // That is still terminal for the caller; without this event it
        // has no way to distinguish failure from a slow connection.
        pending.emplace_back(Disconnected{kServerPeer, reason});
        client_connected = false;
        client_conn = k_HSteamNetConnection_Invalid;
      }
      iface->CloseConnection(info->m_hConn, 0, nullptr, false);
      break;
    }
    default:
      break;
    }
  }
};

} // namespace afterhours::online
