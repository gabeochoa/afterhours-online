#pragma once

// In-process transport. It gives games and tests the same server/client
// topology as a socket backend without sockets, threads, ports, or timing.

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "core.h"

namespace afterhours::online {

struct LocalHub {
  struct Inbox {
    std::mutex mutex;
    std::deque<Event> events;
  };

  std::mutex mutex;
  std::map<PeerId, std::shared_ptr<Inbox>> inboxes;
  PeerId next_peer_id = kFirstRemotePeer;
  bool has_server = false;
};

class LocalTransport final : public Transport {
public:
  explicit LocalTransport(std::shared_ptr<LocalHub> hub)
      : hub_(std::move(hub)) {}

  ~LocalTransport() override { close(); }

  bool listen(std::uint16_t) override {
    if (!hub_ || open_)
      return false;
    std::lock_guard lock(hub_->mutex);
    if (hub_->has_server)
      return false;
    inbox_ = std::make_shared<LocalHub::Inbox>();
    hub_->inboxes[kServerPeer] = inbox_;
    hub_->has_server = true;
    local_id_ = kServerPeer;
    server_ = true;
    open_ = true;
    return true;
  }

  bool connect(std::string_view, std::uint16_t) override {
    if (!hub_ || open_)
      return false;
    PeerId assigned = kInvalidPeer;
    {
      std::lock_guard lock(hub_->mutex);
      if (!hub_->has_server)
        return false;
      assigned = hub_->next_peer_id++;
      inbox_ = std::make_shared<LocalHub::Inbox>();
      hub_->inboxes[assigned] = inbox_;
    }
    local_id_ = assigned;
    server_ = false;
    open_ = true;
    push_to(kServerPeer, Connected{assigned});
    push_to(assigned, Connected{kServerPeer});
    return true;
  }

  void close() override {
    if (!open_ || !hub_)
      return;
    const PeerId closing = local_id_;
    open_ = false;
    {
      std::lock_guard lock(hub_->mutex);
      hub_->inboxes.erase(closing);
      if (server_)
        hub_->has_server = false;
    }
    notify_all(Disconnected{closing}, closing);
    inbox_.reset();
    local_id_ = kInvalidPeer;
    server_ = false;
  }

  bool disconnect(PeerId peer) override {
    if (!open_ || !hub_ || peer == local_id_)
      return false;
    std::shared_ptr<LocalHub::Inbox> dropped;
    {
      std::lock_guard lock(hub_->mutex);
      auto it = hub_->inboxes.find(peer);
      if (it == hub_->inboxes.end())
        return false;
      dropped = it->second;
      hub_->inboxes.erase(it);
    }
    // Mirror a socket close: this endpoint and the dropped peer both
    // observe the disconnect. The dropped peer keeps its inbox object,
    // so the event is still deliverable after hub removal.
    {
      std::lock_guard lock(dropped->mutex);
      dropped->events.emplace_back(Disconnected{local_id_});
    }
    if (inbox_) {
      std::lock_guard lock(inbox_->mutex);
      inbox_->events.emplace_back(Disconnected{peer});
    }
    return true;
  }

  bool send(PeerId to, std::span<const std::byte> payload,
            Channel channel) override {
    if (!open_ || to == kInvalidPeer || to == local_id_)
      return false;
    Packet packet;
    packet.from = local_id_;
    packet.payload.assign(payload.begin(), payload.end());
    packet.channel = channel;
    return push_to(to, Event{std::move(packet)});
  }

  bool broadcast(std::span<const std::byte> payload,
                 Channel channel) override {
    if (!open_)
      return false;
    bool sent = false;
    for (PeerId peer : peer_ids()) {
      if (peer != local_id_)
        sent = send(peer, payload, channel) || sent;
    }
    return sent;
  }

  std::optional<Event> poll() override {
    if (!inbox_)
      return std::nullopt;
    std::lock_guard lock(inbox_->mutex);
    if (inbox_->events.empty())
      return std::nullopt;
    Event event = std::move(inbox_->events.front());
    inbox_->events.pop_front();
    return event;
  }

  [[nodiscard]] PeerId local_peer_id() const override { return local_id_; }
  [[nodiscard]] bool is_server() const override { return server_; }
  [[nodiscard]] bool is_open() const override { return open_; }

private:
  bool push_to(PeerId to, Event event) {
    std::shared_ptr<LocalHub::Inbox> inbox;
    {
      std::lock_guard lock(hub_->mutex);
      auto it = hub_->inboxes.find(to);
      if (it == hub_->inboxes.end())
        return false;
      inbox = it->second;
    }
    std::lock_guard lock(inbox->mutex);
    inbox->events.push_back(std::move(event));
    return true;
  }

  void notify_all(Event event, PeerId except) {
    for (PeerId peer : peer_ids()) {
      if (peer != except)
        push_to(peer, event);
    }
  }

  std::vector<PeerId> peer_ids() {
    std::vector<PeerId> out;
    std::lock_guard lock(hub_->mutex);
    out.reserve(hub_->inboxes.size());
    for (const auto &[peer, _] : hub_->inboxes)
      out.push_back(peer);
    return out;
  }

  std::shared_ptr<LocalHub> hub_;
  std::shared_ptr<LocalHub::Inbox> inbox_;
  PeerId local_id_ = kInvalidPeer;
  bool server_ = false;
  bool open_ = false;
};

} // namespace afterhours::online
