#pragma once

// ECS/session adapter seam (OG-04, gaps item 5, Pharmasea G-P1).
//
// DECISION PROPOSED HERE, flagged for owner review (the trackers call
// this decision "needed", not settled): keep core.h transport-only and
// dependency-free, and put the shared session layer in this header as
// plain bookkeeping + a pump that a game's ECS adapter wraps. This
// header itself has NO afterhours ECS dependency, for the same reason
// core.h has none: a game includes it without changing its link line,
// and this repository does not vendor the ECS headers to compile
// against. An afterhours-native plugin (ProvidesOnlineSession, peer
// components, poll/send systems in the pinned library's plugin shape,
// per gaps item 11) can be built ON this seam later without moving
// the bookkeeping again.
//
// What is shared (both games built it identically): transport event
// pumping in a fixed order, peer<->side bookkeeping with capacity and
// duplicate handling, disconnect cleanup of that bookkeeping. What
// stays in the game: packet schemas and dispatch, handshake message
// contents, snapshot build/apply, ECS writes, menu/HUD state.

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "core.h"

namespace afterhours::online {

// Peer <-> side assignment bookkeeping (Pumpkin's peer_sides map,
// generalized over the game's side type). Sides are handed out in
// the order the game lists them; freeing a side makes it the next
// one handed out again, matching disconnect-then-rejoin behavior.
template <typename Side> class PeerBook {
public:
  explicit PeerBook(std::vector<Side> sides) : sides_(std::move(sides)) {}

  enum class AssignResult { Assigned, AlreadyAssigned, Full };

  // Assign the first free side to a peer.
  [[nodiscard]] AssignResult assign(PeerId peer, Side *assigned = nullptr) {
    if (by_peer_.contains(peer)) {
      if (assigned)
        *assigned = by_peer_.at(peer);
      return AssignResult::AlreadyAssigned;
    }
    for (const Side &side : sides_) {
      if (!by_side_.contains(side)) {
        by_peer_[peer] = side;
        by_side_[side] = peer;
        if (assigned)
          *assigned = side;
        return AssignResult::Assigned;
      }
    }
    return AssignResult::Full;
  }

  [[nodiscard]] bool free_peer(PeerId peer) {
    const auto it = by_peer_.find(peer);
    if (it == by_peer_.end())
      return false;
    by_side_.erase(it->second);
    by_peer_.erase(it);
    return true;
  }

  void clear() { by_peer_.clear(); by_side_.clear(); }

  [[nodiscard]] std::optional<Side> side_of(PeerId peer) const {
    const auto it = by_peer_.find(peer);
    return it == by_peer_.end() ? std::nullopt
                               : std::optional<Side>{it->second};
  }
  [[nodiscard]] std::optional<PeerId> peer_of(Side side) const {
    const auto it = by_side_.find(side);
    return it == by_side_.end() ? std::nullopt
                                : std::optional<PeerId>{it->second};
  }
  [[nodiscard]] std::size_t size() const { return by_peer_.size(); }
  [[nodiscard]] bool full() const { return by_peer_.size() >= sides_.size(); }
  [[nodiscard]] const std::vector<Side> &sides() const { return sides_; }

private:
  std::vector<Side> sides_;
  std::map<PeerId, Side> by_peer_;
  std::map<Side, PeerId> by_side_;
};

// Pumps a Transport into a game handler in the shared order: every
// queued event, in arrival order, with peer-bookkeeping cleanup
// applied BEFORE the game's disconnect callback runs (so the callback
// already sees the freed side). The handler is any type with:
//
//   void on_connected(PeerId);
//   void on_packet(const Packet&);
//   void on_disconnected(PeerId, DisconnectReason, std::optional<Side>);
//
// Side is the game's side enum (use a dummy such as int for games
// that do not assign sides at the session layer). This class owns no
// transport lifetime: the game creates and closes the Transport.
template <typename Side, typename Handler> class SessionDriver {
public:
  SessionDriver(Transport &transport, std::vector<Side> sides,
                Handler &handler)
      : transport_(transport), book_(std::move(sides)), handler_(handler) {}

  // Drain every queued event once. Returns the number drained.
  std::size_t pump() {
    std::size_t drained = 0;
    while (auto event = transport_.poll()) {
      ++drained;
      if (const auto *connected = std::get_if<Connected>(&*event)) {
        handler_.on_connected(connected->peer);
      } else if (const auto *packet = std::get_if<Packet>(&*event)) {
        handler_.on_packet(*packet);
      } else if (const auto *disconnected =
                     std::get_if<Disconnected>(&*event)) {
        const std::optional<Side> side = book_.side_of(disconnected->peer);
        (void)book_.free_peer(disconnected->peer);
        handler_.on_disconnected(disconnected->peer, disconnected->reason,
                                 side);
      }
    }
    return drained;
  }

  [[nodiscard]] PeerBook<Side> &book() { return book_; }
  [[nodiscard]] const PeerBook<Side> &book() const { return book_; }
  [[nodiscard]] Transport &transport() { return transport_; }

private:
  Transport &transport_;
  PeerBook<Side> book_;
  Handler &handler_;
};

} // namespace afterhours::online
