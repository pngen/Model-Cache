#pragma once
// Model Cache — bounded structured event bus.
//
// Subscriptions are bounded. Callbacks are ALWAYS invoked outside any internal
// lock: emit() copies active subscribers to a local vector under a brief lock,
// releases it, then invokes callbacks with no cache lock held, so a subscriber
// can safely call back into the cache without deadlock/reentrancy.

#include "model_cache/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace model_cache {

struct Event {
  EventType type;
  std::uint64_t sequence = 0;      // monotonic
  std::uint64_t timestamp = 0;     // epoch millis
  std::map<std::string, std::string> fields;  // small, bounded key/value
};

using EventCallback = std::function<void(const Event&)>;

class EventBus;

/// A subscription handle; destroying it unsubscribes.
class Subscription {
 public:
  Subscription() = default;
  Subscription(std::uint32_t id, EventBus* bus) : id_(id), bus_(bus) {}
  ~Subscription();
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
  Subscription(Subscription&& o) noexcept
      : id_(o.id_), bus_(o.bus_) { o.bus_ = nullptr; o.id_ = 0; }
  Subscription& operator=(Subscription&& o) noexcept {
    if (this != &o) { unsubscribe(); id_ = o.id_; bus_ = o.bus_; o.bus_ = nullptr; o.id_ = 0; }
    return *this;
  }
  void unsubscribe();
  std::uint32_t id() const noexcept { return id_; }

 private:
  std::uint32_t id_ = 0;
  EventBus* bus_ = nullptr;
};

class EventBus {
 public:
  EventBus(std::uint32_t max_subscriptions, std::uint32_t max_backlog);

  Subscription subscribe(EventCallback cb);
  void unsubscribe(std::uint32_t id);

  /// Emit an event to all current subscribers. Never invoked with cache locks.
  void emit(EventType type, std::map<std::string, std::string> fields = {});

  std::uint64_t sequence() const noexcept { return published_.load(); }
  std::size_t subscriber_count() const;
  std::vector<Event> recent(std::size_t max);

 private:
  struct Entry {
    std::uint32_t id = 0;
    EventCallback cb;
    bool active = true;
  };

  mutable std::mutex mu_;
  std::vector<Entry> subs_;
  std::uint32_t next_id_ = 1;
  std::uint32_t max_subs_;
  std::uint32_t max_backlog_;
  std::vector<Event> backlog_;
  std::uint64_t next_seq_ = 0;
  std::atomic<std::uint64_t> published_;
};

}  // namespace model_cache
